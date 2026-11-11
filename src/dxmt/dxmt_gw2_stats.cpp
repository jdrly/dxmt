#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include <version.h>
#include <windows.h>

namespace dxmt::gw2 {

namespace {
// gw2 (#60): on unless exactly "0". The startup line shows the process's value (it exists only
// with STATS on, where it always said 1).
sw::Switch<bool> kStats{"DXMT_STATS", true, sw::legacy::notZero};
sw::Effective kStatsLine{"DXMT_STATS", [] { return sw::formatValue(statsEnabled()); }};
} // namespace

bool
statsEnabled() {
  static const bool enabled = kStats();
  return enabled;
}

bool
statsReadNow() {
  return kStats();
}

double
hitchMs() {
  static const double ms = [] {
    auto v = env::getEnvVar("DXMT_HITCH_MS");
    return v.empty() ? 50.0 : std::strtod(v.c_str(), nullptr);
  }();
  return ms;
}

// gw2 (#28): QueryPerformanceCounter, not std::chrono::steady_clock. mingw's libstdc++ builds
// steady_clock on gettimeofday (the wall clock: 1 us steps in Wine on macOS, and it can step back,
// which made a scope's unsigned delta wrap). Every clock with sub-ms steps is a crossing into Wine's
// unix side; measured headless under the engine (tests/gw2/clock_bench.zsh, ns per call):
// QueryPerformanceCounter 52.5 (100 ns steps, mach_continuous_time), steady_clock 55.1 (1 us),
// GetSystemTimePreciseAsFileTime 51.2 (1 us, wall clock), clock_gettime(MONOTONIC) 56.5 (QPC).
// KUSER_SHARED_DATA's interrupt time is a 1.4 ns memory read but the wineserver refreshes it only
// when it wakes (every ~18 ms in an idle process), too coarse for a scope.
int64_t
nowNs() {
  static const struct Qpc {
    int64_t freq, ns_per_tick; // ns_per_tick 0: the frequency doesn't divide 1e9 (not Wine's 10 MHz)
    Qpc() {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      freq = f.QuadPart > 0 ? f.QuadPart : 1;
      ns_per_tick = 1'000'000'000 % freq == 0 ? 1'000'000'000 / freq : 0;
    }
  } qpc;
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  if (qpc.ns_per_tick)
    return c.QuadPart * qpc.ns_per_tick;
  return c.QuadPart / qpc.freq * 1'000'000'000 + c.QuadPart % qpc.freq * 1'000'000'000 / qpc.freq;
}

int64_t
clockFloorNs() {
  static const int64_t ns = [] {
    constexpr int kBatches = 9, kReads = 256;
    int64_t mean[kBatches];
    for (auto &m : mean) {
      int64_t t0 = nowNs(), t = t0;
      for (int i = 0; i < kReads; i++)
        t = nowNs();
      m = (t - t0 + kReads / 2) / kReads;
    }
    std::nth_element(mean, mean + kBatches / 2, mean + kBatches); // a preempted batch doesn't count
    return mean[kBatches / 2];
  }();
  return ns;
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
std::atomic<int64_t> g_other_ns[kApiCount];

// gw2 (#28): ApiThread ownership. A thread takes a block on its first lookup and keeps it in a
// fiber-local slot whose callback releases it when the thread exits. A released block has tid 0, so
// no ApiThreadCache can return it, and it is reset only when the next thread takes it: a block is
// never reset while its thread lives. Blocks are never freed (a cache may still point at one).
// The exit callback only stores two atomics: at process exit it runs after NtTerminateProcess has
// killed the other threads (LdrShutdownProcess), so it must not take a lock one of them may hold or
// allocate. Taking a block (never at exit) scans all blocks for a released one under the mutex.
std::mutex g_threads_mutex;
std::vector<ApiThread *> g_all_threads; // every block ever made
std::atomic<DWORD> g_fls = FLS_OUT_OF_INDEXES;
std::atomic<bool> g_fls_failed = false;
// a thread whose block isn't in the fiber-local slot (FlsAlloc or FlsSetValue failed): that block is
// never released and never cached, so no other thread can take or match it. gw2 (#121): it is always
// a new block, never a reclaimed one, which a cache may still point at: the fallback keeps its tid
// after its thread dies, and a thread reusing that id would hit it through such a cache.
thread_local ApiThread *t_thread = nullptr;

void WINAPI
releaseThread(void *block) {
  if (!block)
    return;
  auto t = static_cast<ApiThread *>(block);
  t->tid.store(0, std::memory_order_release);
  t->released.store(true, std::memory_order_release);
}

DWORD
flsIndex() {
  DWORD fls = g_fls.load(std::memory_order_acquire);
  if (fls != FLS_OUT_OF_INDEXES || g_fls_failed.load(std::memory_order_relaxed))
    return fls;
  std::lock_guard lock(g_threads_mutex);
  fls = g_fls.load(std::memory_order_relaxed);
  if (fls == FLS_OUT_OF_INDEXES && !g_fls_failed.load(std::memory_order_relaxed)) {
    fls = FlsAlloc(releaseThread);
    if (fls == FLS_OUT_OF_INDEXES) {
      g_fls_failed.store(true, std::memory_order_relaxed);
      Logger::warn("GW2 PACE: FlsAlloc failed; API time falls back to a TLS lookup per call");
    } else {
      g_fls.store(fls, std::memory_order_release);
    }
  }
  return fls;
}

// fresh: a new block, never one a cache may point at (gw2 #121: for a fallback block)
ApiThread *
takeBlock(uint32_t tid, bool fresh) {
  ApiThread *t = nullptr;
  {
    std::lock_guard lock(g_threads_mutex);
    for (size_t i = 0; !fresh && i < g_all_threads.size(); i++) {
      auto b = g_all_threads[i];
      if (b->released.load(std::memory_order_acquire) && b->released.exchange(false, std::memory_order_acq_rel)) {
        t = b;
        break;
      }
    }
    if (!t) {
      t = new ApiThread();
      g_all_threads.push_back(t);
    }
  }
  t->reset(); // an exited thread's state: this thread starts fresh, as a new TLS block did
  t->tid.store(tid, std::memory_order_release); // from here on, caches return it to this thread only
  return t;
}
} // namespace

void
ApiThread::reset() {
  cacheable = false;
  depth = 0;
  sample_state = 0;
  presenter = false;
  interval = {};
}

uint32_t
ApiThread::draw() {
  uint32_t x = sample_state;
  if (!x) // seeded from the thread id (Knuth's multiplicative hash), never 0
    x = (tid.load(std::memory_order_relaxed) * 2654435761u) | 1u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  sample_state = x;
  return x;
}

void
ApiThread::call(Api api) {
  if (presenter)
    interval.api.calls[(unsigned)api]++;
  else
    g_other_calls[(unsigned)api].fetch_add(1, std::memory_order_relaxed);
}

void
ApiThread::add(Api api, int64_t ns) {
  if (presenter)
    interval.api.ns[(unsigned)api] += ns;
  else
    g_other_ns[(unsigned)api].fetch_add(ns, std::memory_order_relaxed);
}

ApiThread &
apiThread() {
  DWORD fls = flsIndex();
  if (fls != FLS_OUT_OF_INDEXES) {
    if (auto t = static_cast<ApiThread *>(FlsGetValue(fls)))
      return *t;
  }
  if (auto t = t_thread)
    return *t;
  uint32_t tid = this_thread::get_id(); // gw2 (#149): from the TEB, no kernel32 call
  if (fls != FLS_OUT_OF_INDEXES) {
    auto t = takeBlock(tid, false);
    if (FlsSetValue(fls, t)) {
      t->cacheable = true; // released at exit, so a cache may hold it
      return *t;
    }
    // gw2 (#121): t may be a reclaimed block a cache still points at, so it can't be the fallback.
    // Back to the pool (tid 0 again, so no cache returns it); no cache took it from this thread.
    releaseThread(t);
    static std::atomic<bool> warned = false;
    if (!warned.exchange(true))
      Logger::warn("GW2 PACE: FlsSetValue failed; this thread's API time uses a TLS lookup per call");
  }
  // not released at exit: kept per thread, never cached, and new (#121), so no cache points at it
  auto t = takeBlock(tid, true);
  t_thread = t;
  return *t;
}

ApiThread &
ApiThreadCache::get() {
  uint32_t tid = this_thread::get_id(); // gw2 (#149): from the TEB, no kernel32 call
  ApiThread *t = last_.load(std::memory_order_acquire);
  if (t && t->tid.load(std::memory_order_acquire) == tid)
    return *t; // tid is set only by the owning thread and cleared when it exits: this thread's
  t = &apiThread();
  if (t->cacheable) // a block without the exit callback keeps its tid after its thread dies
    last_.store(t, std::memory_order_release);
  return *t;
}

void
apiThreadsDetach() {
  DWORD fls = g_fls.exchange(FLS_OUT_OF_INDEXES);
  if (fls != FLS_OUT_OF_INDEXES)
    FlsFree(fls); // runs releaseThread for every thread's block: the callback mustn't outlive the DLL
}

uint32_t
sampleDraw() {
  return apiThread().draw();
}

void
apiAdd(Api api, int64_t ns) {
  apiThread().add(api, ns);
}

void
apiCall(Api api) {
  apiThread().call(api);
}

void
waitAdd(Wait wait, uint64_t ns) {
  auto &t = apiThread();
  if (!t.presenter)
    return;
  t.interval.wait_ns[(unsigned)wait] += ns;
  t.interval.wait_count[(unsigned)wait]++;
}

ThreadInterval
takeThreadInterval() {
  auto &t = apiThread();
  t.presenter = true;
  ThreadInterval out = t.interval;
  t.interval = {};
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

void
ApiScope::begin(bool sampled, ApiThreadCache *cache) {
  ApiThread &t = cache ? cache->get() : apiThread();
  thread_ = &t;
  if (t.depth++ > 0)
    return; // nested: the outer scope counts
  outer_ = true;
  t.call(api_);
  unsigned bits = sampleBits(api_);
  if (sampled) {
    draw_ = t.draw();
    if ((draw_ >> (32 - bits)) != 0)
      return;
  }
  scale_ = sampled ? 1u << bits : 1;
  t0_ = nowNs();
}

int64_t
ApiScope::stop() {
  ns_ = sinceNs(t0_);
  stopped_ = true;
  return ns_;
}

void
ApiScope::end() {
  thread_->depth--;
  if (!outer_ || !scale_)
    return;
  if (!stopped_)
    stop();
  thread_->add(api_, ns_ * scale_);
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
  // gw2 (#43): two lines, so that neither grows with the other (Wine's 1020-byte line limit)
  Logger::info(str::format("GW2 fork ", DXMT_VERSION, ": ", extra));
  Logger::info(
      str::format("GW2 fork ", DXMT_VERSION, " set:", vars.empty() ? std::string(" none") : vars)
  );
}

} // namespace dxmt::gw2
