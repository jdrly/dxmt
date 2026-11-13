// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Itests/gw2/shim -Isrc/dxmt -Isrc/winemetal tests/gw2/present_section_test.cpp src/dxmt/dxmt_gw2_present.cpp src/dxmt/dxmt_gw2_switches.cpp -o $OUT
//
// gw2 (#224): the PRESENT section (src/dxmt/dxmt_gw2_present.cpp), built natively with the bridge's
// MTLDrawable_trackPresented scripted below. No Metal, no Wine, no real time: every timestamp is made
// up, on CACurrentMediaTime's clock in ns as the bridge gives them.
//
// - fields: drawwait, early and inuse on known times. Three surfaces in turn, so a frame's surface
//   last showed the frame three before it, and that frame left the screen when the one after it was
//   presented. Passes ending before, at and after that moment; a surface seen the first time; a frame
//   without a present pass time; a frame whose GPU times aren't final yet (it waits a window, as
//   aftergpu does); a dropped frame; a gap in the frame numbers, after which every surface is seen
//   anew (a lost frame may have shown on any of them).
// - parity: the fields the section had before #224 (presented, aftergpu, nogpu, missed, starved,
//   target, n, dropped, lost) keep their values. The section of 215e15a is copied below as the
//   oracle, fed the same records and GPU ends window by window (made-up sessions with drops, losses,
//   late GPU ends, the vsync grid, the frame limiter and no grid), and its line must equal the new
//   line up to " drawwait=".
// - presentPassRecord (src/dxmt/dxmt_gw2_commit_record.hpp): the record a chunk notes for its pass.
//
// Run it from the checkout's root (or through tests/gw2/run.zsh).

#include "dxmt_gw2_commit_record.hpp"
#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_present_pace.hpp"
#include "dxmt_gw2_stats.hpp"
#include "winemetal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <format>
#include <map>
#include <random>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s: %s\n", __FILE__, __LINE__, #cond, std::format(__VA_ARGS__).c_str()); \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// --- what the rest of DXMT provides
namespace dxmt::gw2 {
static SectionFn g_present = nullptr;
void
registerSection(const char *name, SectionFn fn) {
  if (std::string(name) == "PRESENT")
    g_present = fn;
}
bool
statsEnabled() {
  return true;
}
} // namespace dxmt::gw2

// the bridge: records the "presented handlers" finished, handed out as the unix side does (at most a
// batch per call, and the losses since the last drain)
static std::deque<WMTPresentedRecord> g_finished;
static uint32_t g_lost_pending = 0;

extern "C" uint32_t
MTLDrawable_trackPresented(obj_handle_t, uint64_t, WMTPresentedRecord *out, uint32_t capacity, uint32_t *lost) {
  uint32_t n = 0;
  while (n < capacity && n < WMT_PRESENTED_BATCH && !g_finished.empty()) {
    out[n++] = g_finished.front();
    g_finished.pop_front();
  }
  *lost = g_lost_pending;
  g_lost_pending = 0;
  return n;
}

// --- the oracle: the PRESENT section of 215e15a, verbatim apart from its names and its input (the
// records and GPU ends it is given, not the bridge's)
namespace oracle {
using dxmt::gw2::skippedRefreshes;
using dxmt::gw2::skipWasQueued;

struct Record {
  uint64_t frame, encode_ns, presented_ns;
};

std::vector<Record> g_pending;
uint64_t g_lost = 0;
std::map<uint64_t, uint64_t> g_gpu_end;
uint64_t g_gpu_latest = 0;
double g_refresh_ms = 0;
uint32_t g_vsyncs = 0;
double g_limiter_ms = 0;
bool g_last_valid = false;
uint64_t g_last_frame = 0, g_last_presented_ns = 0;
double g_late_ms = 0, g_late_limiter_ms = 0;
std::vector<std::pair<uint64_t, uint64_t>> g_gpu_waiting;
constexpr size_t kMaxGpuFrames = 2048;

void
presentGpuEnd(uint64_t frame, uint64_t gpu_end_ns) {
  auto &end = g_gpu_end[frame];
  end = std::max(end, gpu_end_ns);
  g_gpu_latest = std::max(g_gpu_latest, frame);
  while (g_gpu_end.size() > kMaxGpuFrames)
    g_gpu_end.erase(g_gpu_end.begin());
}

std::string
section() {
  std::vector<Record> records;
  uint64_t lost;
  {
    records.swap(g_pending);
    lost = g_lost;
    g_lost = 0;
  }
  if (records.empty() && !lost && g_gpu_waiting.empty())
    return {};
  std::sort(records.begin(), records.end(), [](auto &a, auto &b) { return a.frame < b.frame; });

  double refresh_ms = g_refresh_ms;
  uint32_t vsyncs = g_vsyncs;
  double limiter_ms = g_limiter_ms;
  bool grid = refresh_ms > 0 && vsyncs > 0;
  if (limiter_ms != g_late_limiter_ms) {
    g_late_limiter_ms = limiter_ms;
    g_late_ms = 0;
  }

  std::vector<double> latency;
  latency.reserve(records.size());
  std::vector<std::pair<uint64_t, uint64_t>> join; // (frame, presented_ns): last window's waiting first
  join.swap(g_gpu_waiting);
  size_t waited = join.size();
  uint32_t dropped = 0;
  uint64_t missed = 0, starved = 0;
  for (auto &r : records) {
    if (!r.presented_ns) {
      dropped++;
      g_last_valid = false; // don't pair across a dropped frame
      continue;
    }
    latency.push_back(r.presented_ns > r.encode_ns ? (r.presented_ns - r.encode_ns) / 1e6 : 0.0);
    join.emplace_back(r.frame, r.presented_ns);
    if (grid && g_last_valid && r.frame > g_last_frame && r.presented_ns > g_last_presented_ns) {
      double gap = (r.presented_ns - g_last_presented_ns) / 1e6;
      int64_t skipped;
      if (limiter_ms > 0) {
        // a quarter millisecond short of a refresh, against presentedTime's jitter
        g_late_ms = std::max(0.0, g_late_ms + gap - limiter_ms);
        skipped = (int64_t)std::floor((g_late_ms + 0.25) / refresh_ms);
        g_late_ms = std::max(0.0, g_late_ms - skipped * refresh_ms);
      } else {
        skipped = skippedRefreshes(gap, refresh_ms, vsyncs); // gw2 (#157): shared with LiveSkips
      }
      if (skipped > 0) {
        if (skipWasQueued(r.encode_ns, g_last_presented_ns))
          missed += skipped;
        else
          starved += skipped;
      }
    } else {
      g_late_ms = 0;
    }
    g_last_valid = true;
    g_last_frame = r.frame;
    g_last_presented_ns = r.presented_ns;
  }

  double aftergpu = 0, aftergpu_max = 0;
  uint32_t aftergpu_n = 0, nogpu = 0;
  {
    uint64_t keep_from = 0; // GPU ends below this frame are no longer needed
    for (size_t i = 0; i < join.size(); i++) {
      auto [frame, presented_ns] = join[i];
      keep_from = std::max(keep_from, frame + 1);
      if (frame >= g_gpu_latest && i >= waited) {
        g_gpu_waiting.emplace_back(frame, presented_ns); // the finish thread isn't through it yet
        continue;
      }
      auto it = g_gpu_end.find(frame);
      // a GPU end of 0: none of the frame's command buffers reported one (GPUEndTime 0)
      if (it == g_gpu_end.end() || !it->second || frame >= g_gpu_latest || presented_ns <= it->second) {
        nogpu++;
        continue;
      }
      double v = (presented_ns - it->second) / 1e6;
      aftergpu += v;
      aftergpu_max = std::max(aftergpu_max, v);
      aftergpu_n++;
    }
    for (auto &w : g_gpu_waiting)
      keep_from = std::min(keep_from, w.first);
    g_gpu_end.erase(g_gpu_end.begin(), g_gpu_end.lower_bound(keep_from));
  }

  std::string text;
  if (!latency.empty()) {
    double sum = 0;
    for (double v : latency)
      sum += v;
    std::sort(latency.begin(), latency.end());
    double p95 = latency[std::min(latency.size() - 1, size_t(0.95 * (latency.size() - 1) + 0.5))];
    text += std::format("presented={:.1f}/{:.1f}/{:.1f} ", sum / latency.size(), p95, latency.back());
  } else {
    text += "presented=- ";
  }
  if (aftergpu_n)
    text += std::format("aftergpu={:.1f}/{:.1f} ", aftergpu / aftergpu_n, aftergpu_max);
  else
    text += "aftergpu=- ";
  text += std::format("nogpu={} ", nogpu);
  if (grid && limiter_ms > 0)
    text += std::format("missed={} starved={} target={:.2f}x{:.2f} ", missed, starved, limiter_ms / refresh_ms,
                        refresh_ms);
  else if (grid)
    text += std::format("missed={} starved={} target={}x{:.2f} ", missed, starved, vsyncs, refresh_ms);
  else
    text += "missed=- starved=- target=- ";
  text += std::format("n={} dropped={} lost={}", records.size(), dropped, lost);
  return text;
}
} // namespace oracle

// --- driving both

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kRefresh = 16'666'667;

// the records of one window reach the section through presentTrack, as the encode thread collects them
static void
deliver(const std::vector<WMTPresentedRecord> &records, uint32_t lost = 0) {
  for (auto &r : records) {
    g_finished.push_back(r);
    oracle::g_pending.push_back({r.frame, r.encode_ns, r.presented_ns});
  }
  g_lost_pending += lost;
  oracle::g_lost += lost;
  while (!g_finished.empty() || g_lost_pending)
    dxmt::gw2::presentTrack(0, 0);
}

static void
gpuEnd(uint64_t frame, uint64_t end_ns) {
  dxmt::gw2::presentGpuEnd(frame, end_ns);
  oracle::presentGpuEnd(frame, end_ns);
}

static void
pacing(double refresh_ms, uint32_t vsyncs, double limiter_ms) {
  dxmt::gw2::presentSetPacing(refresh_ms, vsyncs, limiter_ms);
  oracle::g_refresh_ms = refresh_ms;
  oracle::g_vsyncs = vsyncs;
  oracle::g_limiter_ms = limiter_ms;
}

// one window: the new line, checked against the oracle's up to the new fields
static std::string
window(const std::string &what) {
  std::string now = dxmt::gw2::g_present ? dxmt::gw2::g_present(2000, 0) : std::string("(no section)");
  std::string old = oracle::section();
  auto at = now.find(" drawwait=");
  std::string kept = at == std::string::npos ? now : now.substr(0, at);
  CHECK(kept == old, "{}: the fields before #224 changed\n  now: {}\n  old: {}", what, now, old);
  CHECK(now.empty() == (at == std::string::npos), "{}: a line without the new fields: {}", what, now);
  return now;
}

static std::string
field(const std::string &line, const std::string &name) {
  auto at = line.find(" " + name + "=");
  if (at == std::string::npos)
    return "(none)";
  at += name.size() + 2;
  return line.substr(at, line.find(' ', at) - at);
}

static WMTPresentedRecord
rec(uint64_t frame, uint64_t encode_ns, uint64_t presented_ns, uint64_t surface, bool in_use = false) {
  return {frame, encode_ns, presented_ns, surface, in_use ? 1u : 0u};
}

// frame f of a locked 60 with three drawables: presented at base + f refreshes, encoded three
// refreshes earlier (less 1 ms); its surface last showed f - 3, which left the screen when f - 2 was
// presented
static void
fields() {
  pacing(1000.0 / 60, 1, 0);
  const uint64_t base = 1'000'000 * kMs;
  auto presented = [&](uint64_t f) { return base + f * kRefresh; };
  auto encoded = [&](uint64_t f) { return presented(f) - 3 * kRefresh + kMs; };
  const uint64_t surfaces[3] = {101, 102, 103};

  // frames 1..9: 1-3 first sight of each surface; then by frame:
  //   4: pass ends 2 ms before frame 2 was presented (early)
  //   5: pass ends exactly when frame 3 was presented (not early: not before)
  //   6: pass ends 1 ms after frame 4 was presented (not early)
  //   7: no present pass time (out of drawwait and early)
  //   8: early again, its surface in use when taken
  //   9: early, but its GPU times come a window late
  std::vector<WMTPresentedRecord> records;
  for (uint64_t f = 1; f <= 9; f++)
    records.push_back(rec(f, encoded(f), presented(f), surfaces[(f - 1) % 3], f == 8));
  auto pass = [&](uint64_t f, int64_t end_vs_replaced_ns, uint64_t start_after_encode_ns) {
    uint64_t replaced = presented(f - 2);
    dxmt::gw2::presentPassGpu(f, encoded(f) + start_after_encode_ns, replaced + end_vs_replaced_ns);
  };
  // first sight: early unknown, drawwait counts (0.5 ms each)
  for (uint64_t f = 1; f <= 3; f++)
    dxmt::gw2::presentPassGpu(f, encoded(f) + kMs / 2, encoded(f) + kMs);
  pass(4, -2 * int64_t(kMs), 1 * kMs);  // drawwait 1.0
  pass(5, 0, 2 * kMs);                  // drawwait 2.0
  pass(6, int64_t(kMs), 16 * kMs);      // drawwait 16.0
  pass(8, -int64_t(kMs), 3 * kMs);      // drawwait 3.0
  for (uint64_t f = 1; f <= 9; f++)
    gpuEnd(f, presented(f) - 30 * kMs);
  deliver(records);
  // frames 1..8 final; 9 is the newest with a GPU end, so the finish thread may not be through it: it
  // waits a window
  auto line = window("fields, window 1");
  std::printf("fields 1: %s\n", line.c_str());
  // drawwait over 1,2,3 (0.5 each), 4 (1), 5 (2), 6 (16), 8 (3): 23.5 / 7 = 3.36, max 16
  CHECK(field(line, "drawwait") == "3.4/16.0", "drawwait {}", field(line, "drawwait"));
  CHECK(field(line, "early") == "2", "early {} (frames 4 and 8)", field(line, "early"));
  CHECK(field(line, "inuse") == "1", "inuse {}", field(line, "inuse"));

  // window 2: frame 9's pass times arrive (early: 0.5 ms before frame 7 was presented), and frame 10's
  // GPU end, so 9 is final; frame 10 waits
  pass(9, -int64_t(kMs) / 2, 1 * kMs);
  gpuEnd(10, presented(10) - 30 * kMs);
  deliver({rec(10, encoded(10), presented(10), surfaces[0])});
  line = window("fields, window 2");
  std::printf("fields 2: %s\n", line.c_str());
  CHECK(field(line, "drawwait") == "1.0/1.0", "drawwait {} (frame 9 only)", field(line, "drawwait"));
  CHECK(field(line, "early") == "1", "early {} (frame 9)", field(line, "early"));
  CHECK(field(line, "inuse") == "0", "inuse {}", field(line, "inuse"));

  // window 3: frame 11's record is lost, then 12 to 15, 14 dropped. The gap starts every surface's
  // history over: the lost frame may have shown on any of them.
  // - 10 (waiting since window 2): its pass ends exactly when frame 8 was presented: not early.
  // - 12 (surface 103) and 13 (surface 101): first seen after the gap: unknown, not counted, though
  //   both passes end before the times their surfaces' history from before the gap would give.
  // - 14 is dropped: it never shows, so 13 stays on screen until 15.
  // - 15 (surface 103): it last showed 12, which left when 13 was presented: early.
  pass(10, 0, 0);
  pass(12, -int64_t(kMs), 0);
  dxmt::gw2::presentPassGpu(13, encoded(13), presented(11) - kMs);
  dxmt::gw2::presentPassGpu(15, encoded(15), presented(13) - kMs);
  for (uint64_t f = 11; f <= 16; f++)
    gpuEnd(f, presented(f) - 30 * kMs);
  auto surface = [&](uint64_t f) { return surfaces[(f - 1) % 3]; };
  deliver({rec(12, encoded(12), presented(12), surface(12)), rec(13, encoded(13), presented(13), surface(13)),
           rec(14, encoded(14), 0, surface(14)), rec(15, encoded(15), presented(15), surface(15))});
  line = window("fields, window 3");
  std::printf("fields 3: %s\n", line.c_str());
  CHECK(field(line, "early") == "1", "early {} (frame 15)", field(line, "early"));
  CHECK(field(line, "drawwait") == "0.0/0.0", "drawwait {} (frames 10, 12, 13, 15 start at the encode)",
        field(line, "drawwait"));

  // window 4: frame 16's record is lost, and 16 showed on surface 101, which last showed 13 before it
  // (replaced at presented(15)). Frame 19 shows on 101 again. Measured against 101's history from
  // before the gap, its pass (made to end before presented(15)) would count early; after the gap 101
  // is seen anew, so it doesn't count.
  dxmt::gw2::presentPassGpu(19, encoded(19), presented(15) - kMs);
  for (uint64_t f = 17; f <= 20; f++)
    gpuEnd(f, presented(f) - 30 * kMs);
  deliver({rec(17, encoded(17), presented(17), surface(17)), rec(18, encoded(18), presented(18), surface(18)),
           rec(19, encoded(19), presented(19), surface(19))});
  line = window("fields, window 4");
  std::printf("fields 4: %s\n", line.c_str());
  CHECK(field(line, "early") == "0", "early {} (frame 19 after the gap: its surface seen anew)",
        field(line, "early"));
}

// made-up sessions: the new section and the oracle get the same records and GPU ends each window
static void
parity() {
  std::mt19937_64 rng(224);
  auto chance = [&](int percent) { return int(rng() % 100) < percent; };
  struct Pacing {
    double refresh_ms;
    uint32_t vsyncs;
    double limiter_ms;
  };
  const Pacing pacings[] = {{1000.0 / 60, 1, 0}, {1000.0 / 60, 2, 0}, {1000.0 / 60, 1, 1000.0 / 45}, {0, 0, 0},
                            {1000.0 / 120, 1, 0}};
  uint64_t frame = 100'000, t = 5'000'000 * kMs, gpu_frame = frame;
  int windows = 0;
  for (auto &p : pacings) {
    pacing(p.refresh_ms, p.vsyncs, p.limiter_ms);
    uint64_t step = p.refresh_ms > 0 ? uint64_t(p.refresh_ms * 1e6) : 20 * kMs;
    for (int w = 0; w < 40; w++, windows++) {
      std::vector<WMTPresentedRecord> records;
      uint32_t lost = chance(10) ? uint32_t(rng() % 3) + 1 : 0;
      int n = chance(5) ? 0 : int(rng() % 40) + 1;
      for (int i = 0; i < n; i++) {
        frame += chance(5) ? 2 : 1; // now and then a frame without a record
        t += step * (chance(15) ? 2 + rng() % 2 : 1) + rng() % 400'000;
        uint64_t encode = t - (rng() % 3 + 1) * step - rng() % (2 * kMs);
        uint64_t presented = chance(4) ? 0 : t;
        records.push_back(rec(frame, encode, presented, 1 + rng() % 5, chance(30)));
        if (chance(60))
          dxmt::gw2::presentPassGpu(frame, encode + rng() % (18 * kMs), t - rng() % (20 * kMs));
      }
      std::shuffle(records.begin(), records.end(), rng); // the ring's order isn't frame order
      // the finish thread: mostly through the window's frames, sometimes behind; some ends late or 0
      uint64_t through = frame - (chance(30) ? rng() % 4 : 0);
      for (; gpu_frame <= through; gpu_frame++) {
        if (chance(5))
          continue; // no GPU end for this frame
        uint64_t end = chance(3) ? 0 : t - (rng() % 60) * kMs + (chance(5) ? 40 * kMs : 0);
        gpuEnd(gpu_frame, end);
        if (chance(10))
          gpuEnd(gpu_frame, end + kMs); // a second chunk of the frame
      }
      deliver(records, lost);
      window(std::format("parity window {}", windows));
    }
  }
  std::printf("parity: %d windows\n", windows);
}

// gw2 (#242): presentat, last on the line once Metal 4 counted a present (avg/max ms of the window); no
// field without one, and the window starts over. Runs last: the oracle doesn't know the field.
static void
presentAt() {
  std::string line = dxmt::gw2::g_present(2000, 0);
  CHECK(line.find("presentat=") == std::string::npos, "presentat without a present: {}", line);
  dxmt::gw2::presentAtSample(0); // the Metal 4 order
  dxmt::gw2::presentAtSample(3'500'000);
  dxmt::gw2::presentAtSample(1'000'000);
  line = dxmt::gw2::g_present(2000, 0);
  CHECK(line.ends_with(" presentat=1.50/3.50"), "presentat of 0, 3.5 and 1 ms: {}", line);
  line = dxmt::gw2::g_present(2000, 0);
  CHECK(line.find("presentat=") == std::string::npos, "presentat after its window: {}", line);
}

static void
passRecord() {
  using dxmt::gw2::kNoPresentCommit;
  using dxmt::gw2::kPresentLastCommit;
  using dxmt::gw2::presentPassRecord;
  std::vector<int> records = {10, 11, 12};
  std::span<const int> s(records);
  CHECK(presentPassRecord(s, kNoPresentCommit) == nullptr, "none noted");
  CHECK(presentPassRecord(s, kPresentLastCommit) == &records[2], "Metal 3: the last");
  CHECK(presentPassRecord(s, 1) == &records[1], "Metal 4: ordinal 1");
  CHECK(presentPassRecord(s, 3) == nullptr, "an ordinal past the records");
  CHECK(presentPassRecord(std::span<const int>(), kPresentLastCommit) == nullptr, "no records");
}

int
main() {
  CHECK(dxmt::gw2::g_present != nullptr, "the PRESENT section registered");
  if (!dxmt::gw2::g_present)
    return 1;
  passRecord();
  fields();
  parity();
  presentAt();
  if (failures) {
    std::printf("present_section_test: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("present_section_test: all %d checks passed\n", checks);
  return 0;
}
