#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <version.h>
#include <windows.h>

namespace dxmt::gw2 {

bool
statsEnabled() {
  static const bool enabled = env::getEnvVar("DXMT_STATS") != "0";
  return enabled;
}

double
hitchMs() {
  static const double ms = [] {
    auto v = env::getEnvVar("DXMT_HITCH_MS");
    return v.empty() ? 50.0 : std::strtod(v.c_str(), nullptr);
  }();
  return ms;
}

int64_t
nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()
  )
      .count();
}

// --- sections

namespace {
struct Section {
  const char *name;
  SectionFn fn;
};
std::vector<Section> &
sections() {
  static std::vector<Section> list;
  return list;
}
std::mutex &
sectionsMutex() {
  static std::mutex m;
  return m;
}
} // namespace

void
registerSection(const char *name, SectionFn fn) {
  std::lock_guard lock(sectionsMutex());
  sections().push_back({name, fn});
}

void
emitSections(double window_ms, uint32_t frames) {
  std::lock_guard lock(sectionsMutex());
  for (auto &s : sections()) {
    auto text = s.fn(window_ms, frames);
    if (!text.empty())
      Logger::info(str::format(s.name, " ", text));
  }
}

// --- API time

const char *const kApiNames[kApiCount] = {"present", "map", "flush", "create", "draw", "dispatch", "presentwait"};

namespace {
std::atomic<uint64_t> g_other_calls[kApiCount];
std::atomic<uint64_t> g_other_ns[kApiCount];
thread_local bool t_presenter = false;
thread_local int t_depth = 0;
thread_local uint32_t t_sample_counter[kApiCount]; // per category: one can't use up another's samples
thread_local ThreadInterval t_interval;
constexpr uint32_t kSampleEvery = 16;
} // namespace

void
apiAdd(Api api, uint64_t ns) {
  if (t_presenter)
    t_interval.api.ns[(unsigned)api] += ns;
  else
    g_other_ns[(unsigned)api].fetch_add(ns, std::memory_order_relaxed);
}

void
apiCall(Api api) {
  if (t_presenter)
    t_interval.api.calls[(unsigned)api]++;
  else
    g_other_calls[(unsigned)api].fetch_add(1, std::memory_order_relaxed);
}

void
waitAdd(Wait wait, uint64_t ns) {
  if (!t_presenter)
    return;
  t_interval.wait_ns[(unsigned)wait] += ns;
  t_interval.wait_count[(unsigned)wait]++;
}

ThreadInterval
takeThreadInterval() {
  t_presenter = true;
  ThreadInterval out = t_interval;
  t_interval = {};
  return out;
}

ApiTotals
otherDrain() {
  ApiTotals t;
  for (unsigned i = 0; i < kApiCount; i++) {
    t.calls[i] = g_other_calls[i].exchange(0, std::memory_order_relaxed);
    t.ns[i] = g_other_ns[i].exchange(0, std::memory_order_relaxed);
  }
  return t;
}

ApiScope::ApiScope(Api api, bool sampled) : api_(api) {
  if (!statsEnabled())
    return;
  if (t_depth++ > 0)
    return; // nested: the outer scope counts
  outer_ = true;
  apiCall(api);
  if (sampled && (t_sample_counter[(unsigned)api]++ % kSampleEvery) != 0)
    return;
  scale_ = sampled ? kSampleEvery : 1;
  t0_ = nowNs();
}

ApiScope::~ApiScope() {
  if (!statsEnabled())
    return;
  t_depth--;
  if (!outer_ || !scale_)
    return;
  apiAdd(api_, uint64_t(nowNs() - t0_) * scale_);
}

// --- GPU spans

namespace {
constexpr size_t kMaxQueued = 1024;
} // namespace

void
GpuCollector::finalize() {
  if (frame_ == ~0ull)
    return;
  GpuFrame f{frame_, 0, ~0ull, 0, 0, uint32_t(spans_.size()) + missing_, missing_, false, false};
  std::sort(spans_.begin(), spans_.end());
  uint64_t cs = 0, ce = 0;
  bool open = false;
  for (auto [s, e] : spans_) {
    f.first_ns = std::min(f.first_ns, s);
    f.last_ns = std::max(f.last_ns, e);
    if (!open) {
      cs = s, ce = e, open = true;
    } else if (s > ce) {
      f.span_ns += ce - cs;
      cs = s, ce = e;
    } else {
      ce = std::max(ce, e);
    }
  }
  if (open)
    f.span_ns += ce - cs;
  if (spans_.empty())
    f.first_ns = f.last_ns = 0;
  // A frame with a buffer without timestamps has an unknown span and unknown ends: it is neither
  // averaged nor used as either end of a gap.
  f.complete = open && missing_ == 0;
  if (!f.complete) {
    f.span_ns = 0;
    prev_last_ns_ = 0;
  } else {
    if (prev_last_ns_) {
      f.has_gap = true;
      f.gap_ns = f.first_ns > prev_last_ns_ ? f.first_ns - prev_last_ns_ : 0;
    }
    prev_last_ns_ = std::max(prev_last_ns_, f.last_ns);
  }
  spans_.clear();
  missing_ = 0;
  std::lock_guard lock(mutex_);
  if (done_.size() >= kMaxQueued) {
    done_.pop_front();
    dropped_++;
  }
  done_.push_back(f);
}

void
GpuCollector::add(uint64_t frame, uint64_t gpu_start_ns, uint64_t gpu_end_ns) {
  if (frame != frame_) {
    finalize();
    frame_ = frame;
  }
  if (gpu_start_ns == 0 || gpu_end_ns < gpu_start_ns)
    missing_++;
  else
    spans_.emplace_back(gpu_start_ns, gpu_end_ns);
}

std::vector<GpuFrame>
GpuCollector::drain(uint32_t &dropped) {
  std::lock_guard lock(mutex_);
  std::vector<GpuFrame> out(done_.begin(), done_.end());
  done_.clear();
  dropped = dropped_;
  dropped_ = 0;
  return out;
}

// --- startup line

bool
takeStartup() {
  static std::atomic<bool> done = false;
  return !done.exchange(true);
}

void
logStartup(const std::string &extra) {
  std::string vars;
  if (auto block = GetEnvironmentStringsA()) {
    for (const char *p = block; *p; p += strlen(p) + 1) {
      if (!strncmp(p, "DXMT_", 5) || !strncmp(p, "WINENCPU=", 9) || !strncmp(p, "WINEMSYNC=", 10)) {
        vars += " ";
        vars += p;
      }
    }
    FreeEnvironmentStringsA(block);
  }
  Logger::info(str::format(
      "GW2 fork ", DXMT_VERSION, ": ", extra, " set:", vars.empty() ? std::string(" none") : vars
  ));
}

} // namespace dxmt::gw2
