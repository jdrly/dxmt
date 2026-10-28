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

const char *const kApiNames[kApiCount] = {"present", "map", "flush", "create", "draw", "dispatch"};

namespace {
std::atomic<uint64_t> g_calls[kApiCount];
std::atomic<uint64_t> g_ns[kApiCount];
thread_local int t_depth = 0;
thread_local uint32_t t_sample_counter = 0;
thread_local uint64_t t_frame_ns[kApiCount];
thread_local uint64_t t_frame_calls[kApiCount];
constexpr uint32_t kSampleEvery = 16;
} // namespace

void
apiAdd(Api api, uint64_t ns) {
  g_ns[(unsigned)api].fetch_add(ns, std::memory_order_relaxed);
  t_frame_ns[(unsigned)api] += ns;
}

void
apiCall(Api api) {
  g_calls[(unsigned)api].fetch_add(1, std::memory_order_relaxed);
  t_frame_calls[(unsigned)api]++;
}

ApiScope::ApiScope(Api api, bool sampled) : api_(api) {
  if (!statsEnabled())
    return;
  if (t_depth++ > 0)
    return; // nested: the outer scope counts
  outer_ = true;
  g_calls[(unsigned)api].fetch_add(1, std::memory_order_relaxed);
  t_frame_calls[(unsigned)api]++;
  if (sampled && (t_sample_counter++ % kSampleEvery) != 0)
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

ApiTotals
apiDrain() {
  ApiTotals t;
  for (unsigned i = 0; i < kApiCount; i++) {
    t.calls[i] = g_calls[i].exchange(0, std::memory_order_relaxed);
    t.ns[i] = g_ns[i].exchange(0, std::memory_order_relaxed);
  }
  return t;
}

ApiTotals
apiFrameOfThisThread() {
  ApiTotals t;
  for (unsigned i = 0; i < kApiCount; i++) {
    t.calls[i] = t_frame_calls[i];
    t.ns[i] = t_frame_ns[i];
  }
  return t;
}

void
apiFrameReset() {
  for (unsigned i = 0; i < kApiCount; i++) {
    t_frame_calls[i] = 0;
    t_frame_ns[i] = 0;
  }
}

// --- GPU spans

namespace {
constexpr size_t kMaxQueued = 1024;
std::mutex g_gpu_mutex;
std::deque<GpuFrame> g_gpu_done;
// finish-thread state: the frame being collected
uint64_t g_gpu_frame = ~0ull;
std::vector<std::pair<uint64_t, uint64_t>> g_gpu_spans;
uint32_t g_gpu_missing = 0;

void
finalizeGpuFrame() {
  if (g_gpu_frame == ~0ull)
    return;
  GpuFrame f{g_gpu_frame, 0, ~0ull, 0, uint32_t(g_gpu_spans.size()) + g_gpu_missing, g_gpu_missing};
  std::sort(g_gpu_spans.begin(), g_gpu_spans.end());
  uint64_t cs = 0, ce = 0;
  bool open = false;
  for (auto [s, e] : g_gpu_spans) {
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
  if (g_gpu_spans.empty())
    f.first_ns = f.last_ns = 0;
  g_gpu_spans.clear();
  g_gpu_missing = 0;
  std::lock_guard lock(g_gpu_mutex);
  if (g_gpu_done.size() >= kMaxQueued)
    g_gpu_done.pop_front();
  g_gpu_done.push_back(f);
}
} // namespace

void
gpuSpanAdd(uint64_t frame, uint64_t gpu_start_ns, uint64_t gpu_end_ns) {
  if (!statsEnabled())
    return;
  if (frame != g_gpu_frame) {
    finalizeGpuFrame();
    g_gpu_frame = frame;
  }
  if (gpu_start_ns == 0 || gpu_end_ns < gpu_start_ns)
    g_gpu_missing++;
  else
    g_gpu_spans.emplace_back(gpu_start_ns, gpu_end_ns);
}

std::vector<GpuFrame>
gpuSpanDrain() {
  std::lock_guard lock(g_gpu_mutex);
  std::vector<GpuFrame> out(g_gpu_done.begin(), g_gpu_done.end());
  g_gpu_done.clear();
  return out;
}

// --- app pacing

namespace {
std::mutex g_app_mutex;
std::vector<double> g_app_intervals;
int64_t g_app_last = 0;
} // namespace

double
appPresent() {
  if (!statsEnabled())
    return 0;
  auto now = nowNs();
  std::lock_guard lock(g_app_mutex);
  double ms = 0;
  if (g_app_last) {
    ms = (now - g_app_last) / 1e6;
    if (g_app_intervals.size() < 16384)
      g_app_intervals.push_back(ms);
  }
  g_app_last = now;
  return ms;
}

std::vector<double>
appDrain() {
  std::lock_guard lock(g_app_mutex);
  std::vector<double> out;
  out.swap(g_app_intervals);
  return out;
}

// --- startup line

void
logStartup(const std::string &extra) {
  static std::atomic<bool> done = false;
  if (done.exchange(true))
    return;
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
      "GW2 fork ", DXMT_VERSION, ":", vars.empty() ? std::string(" (no DXMT_* set)") : vars, " ", extra
  ));
}

} // namespace dxmt::gw2
