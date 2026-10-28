#include "dxmt_gw2_timestamp.hpp"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>
#include <format>
#include <string>

namespace dxmt::gw2 {

namespace {

using counter = std::atomic<uint64_t>;

constexpr unsigned kTsCount = (unsigned)TsCount::Count;
counter g_counts[kTsCount];
counter g_clamped;
// tsspan: per frame, the last resolved value minus the first (what the game sees from its first
// timestamp of the frame to its last)
counter g_span_sum_ns, g_span_frames, g_span_max_ns;

inline uint64_t
take(counter &total) {
  return total.exchange(0, std::memory_order_relaxed);
}

// The finish thread's frame being resolved; its span is published when the next frame's first query
// arrives (so it lands one frame late).
struct SpanState {
  uint64_t frame = ~0ull;
  uint64_t first = 0;
  uint64_t last = 0;
};
thread_local SpanState t_span;

void
publishSpan(const SpanState &s) {
  if (s.frame == ~0ull)
    return;
  uint64_t span = s.last - s.first;
  g_span_sum_ns.fetch_add(span, std::memory_order_relaxed);
  g_span_frames.fetch_add(1, std::memory_order_relaxed);
  uint64_t max = g_span_max_ns.load(std::memory_order_relaxed);
  while (span > max && !g_span_max_ns.compare_exchange_weak(max, span, std::memory_order_relaxed)) {
  }
}

std::string
timestampSection(double, uint32_t frames) {
  uint64_t counts[kTsCount];
  for (unsigned i = 0; i < kTsCount; i++)
    counts[i] = take(g_counts[i]);
  uint64_t clamped = take(g_clamped);
  uint64_t span_sum = take(g_span_sum_ns), span_frames = take(g_span_frames), span_max = take(g_span_max_ns);
  if (!frames || (!counts[(unsigned)TsCount::End] && !counts[(unsigned)TsCount::Disjoint] && !span_frames))
    return {};
  double f = frames;
  std::string out = std::format(
      "mode={} ends={:.1f} disjoint={:.1f} kept={:.1f} saved={:.1f} clamped={:.1f}", timestampModeName(timestampMode()),
      counts[(unsigned)TsCount::End] / f, counts[(unsigned)TsCount::Disjoint] / f, counts[(unsigned)TsCount::Kept] / f,
      counts[(unsigned)TsCount::Saved] / f, clamped / f
  );
  if (span_frames)
    out += std::format(" tsspan={:.2f}/{:.1f}", span_sum / 1e6 / span_frames, span_max / 1e6);
  else
    out += " tsspan=unavailable";
  return out;
}

SectionRegistrar s_timestamp("TIMESTAMP", &timestampSection);

} // namespace

const char *
timestampModeName(TimestampMode mode) {
  switch (mode) {
  case TimestampMode::Exact:
    return "exact";
  case TimestampMode::Estimate:
    return "estimate";
  case TimestampMode::Boundary:
    return "boundary";
  }
  return "estimate";
}

TimestampMode
timestampMode() {
  static const TimestampMode mode = [] {
    auto v = env::getEnvVar("DXMT_TIMESTAMP_MODE");
    TimestampMode m = TimestampMode::Estimate;
    if (v == "exact" || v == "0")
      m = TimestampMode::Exact;
    else if (v == "boundary")
      m = TimestampMode::Boundary;
    else if (!v.empty() && v != "estimate")
      Logger::warn(std::format("DXMT_TIMESTAMP_MODE={} is not exact, estimate or boundary; using estimate", v));
    switch (m) {
    case TimestampMode::Exact:
      Logger::info("Timestamp queries: exact (DXMT_TIMESTAMP_MODE): each End closes the open pass and commits at "
                   "the next pass change; the value is that command buffer's GPU end");
      break;
    case TimestampMode::Estimate:
      Logger::info("Timestamp queries: estimate (DXMT_TIMESTAMP_MODE): no pass split and no commit; the value is "
                   "interpolated between the GPU start and end of the command buffer the query lands in");
      break;
    case TimestampMode::Boundary:
      Logger::info("Timestamp queries: boundary (DXMT_TIMESTAMP_MODE): no pass split; the commit happens at the next "
                   "pass change; the value is interpolated in that command buffer");
      break;
    }
    return m;
  }();
  return mode;
}

void
timestampActed() {
  static std::atomic<bool> logged{false};
  if (logged.load(std::memory_order_relaxed) || logged.exchange(true, std::memory_order_relaxed))
    return;
  Logger::info(std::format(
      "Timestamp queries: the first query End in {} mode kept its pass open{}", timestampModeName(timestampMode()),
      timestampMode() == TimestampMode::Estimate ? " and requested no commit" : ""
  ));
}

void
timestampCount(TsCount what) {
  g_counts[(unsigned)what].fetch_add(1, std::memory_order_relaxed);
}

void
timestampResolved(uint64_t frame, uint64_t value, bool clamped) {
  if (clamped)
    g_clamped.fetch_add(1, std::memory_order_relaxed);
  auto &s = t_span;
  if (s.frame != frame) {
    publishSpan(s);
    s = {.frame = frame, .first = value, .last = value};
    return;
  }
  s.last = value;
}

uint64_t &
timestampLatest() {
  // only finish threads resolve queries (and the device's destruction, on its own thread)
  thread_local uint64_t latest = 0;
  return latest;
}

} // namespace dxmt::gw2
