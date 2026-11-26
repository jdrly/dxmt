#include "dxmt_gw2_compile.hpp"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "winemetal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>

namespace dxmt::gw2 {

namespace {

void
atomicMax(std::atomic<uint64_t> &a, uint64_t v) {
  auto cur = a.load(std::memory_order_relaxed);
  while (cur < v && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

// --- counters (all threads), kept twice: the COMPILE section drains the window's, the encode
// thread drains the frame's at each frame boundary (for the COMPILE frame= line)

struct Counts {
  std::atomic<uint64_t> variants, air_hit, air_miss, pso_hit, pso_miss, built, failed, shader_ns, pso_ns;
};

Counts g_window, g_frame;

void
bump(std::atomic<uint64_t> Counts::*field, uint64_t v = 1) {
  (g_window.*field).fetch_add(v, std::memory_order_relaxed);
  (g_frame.*field).fetch_add(v, std::memory_order_relaxed);
}

std::atomic<uint64_t> g_shader_max_ns, g_pso_max_ns;
std::atomic<uint64_t> g_wait_n, g_wait_ns, g_wait_max_ns, g_slow_frames;
std::atomic<int64_t> g_queue;
std::atomic<uint64_t> g_queue_max, g_threads;

// --- the encoding thread's current frame

thread_local uint64_t t_frame = ~0ull;
thread_local uint64_t t_wait_n = 0, t_wait_ns = 0, t_wait_max_ns = 0;
thread_local int64_t t_last_line_ns = 0;
thread_local uint64_t t_dropped = 0;

double
waitLineMs() {
  return compileSettings().wait_ms;
}

constexpr int64_t kLineIntervalNs = 200'000'000; // at most 5 COMPILE frame lines per second

void
finishFrame() {
  // What all threads did while the encode thread was on this frame (the render thread may already
  // record a later one): drained at every boundary, so it never spans frames.
  auto x = [](std::atomic<uint64_t> Counts::*field) {
    return (g_frame.*field).exchange(0, std::memory_order_relaxed);
  };
  uint64_t variants = x(&Counts::variants), air_hit = x(&Counts::air_hit), air_miss = x(&Counts::air_miss),
           pso_hit = x(&Counts::pso_hit), pso_miss = x(&Counts::pso_miss), built = x(&Counts::built),
           failed = x(&Counts::failed), shader_ns = x(&Counts::shader_ns), pso_ns = x(&Counts::pso_ns);
  if (t_frame == ~0ull || !t_wait_n)
    return;
  double threshold = waitLineMs();
  double ms = t_wait_ns / 1e6;
  if (threshold > 0 && ms >= threshold) {
    g_slow_frames.fetch_add(1, std::memory_order_relaxed);
    auto now = nowNs();
    if (now - t_last_line_ns >= kLineIntervalNs) {
      Logger::info(std::format(
          "COMPILE frame={} wait={}x {:.1f}/{:.1f} queue={} variants={} air={}/{} pso={}/{} built={} failed={} "
          "build=shader:{:.0f} pipeline:{:.0f}{}",
          t_frame, t_wait_n, ms, t_wait_max_ns / 1e6, std::max<int64_t>(0, g_queue.load(std::memory_order_relaxed)),
          variants, air_hit, air_miss, pso_hit, pso_miss, built, failed, shader_ns / 1e6, pso_ns / 1e6,
          t_dropped ? std::format(" dropped={}", t_dropped) : std::string()
      ));
      t_last_line_ns = now;
      t_dropped = 0;
    } else {
      t_dropped++;
    }
  }
  t_wait_n = t_wait_ns = t_wait_max_ns = 0;
}

// --- PSO archives

constexpr uint32_t kKeepSessions = 4;
constexpr uint32_t kMaxPipelines = 8192;
constexpr auto kFlushPeriod = std::chrono::seconds(120);

struct ArchiveState {
  std::atomic<bool> on = false;
  std::atomic<uint64_t> built = 0;
  dxmt::mutex mutex;
  dxmt::condition_variable cond;
  bool requested = false;
  // window deltas for the COMPILE section
  WMTPSOArchiveStats last = {};
};

ArchiveState &
archive() {
  // Never destroyed: its flush thread is never joined, so exit never waits for a flush.
  static auto *state = new ArchiveState;
  return *state;
}

void
logReport(const char *report) {
  std::string_view text(report);
  while (!text.empty()) {
    auto end = text.find('\n');
    auto line = text.substr(0, end);
    if (!line.empty())
      Logger::info(std::string(line));
    if (end == std::string_view::npos)
      break;
    text.remove_prefix(end + 1);
  }
}

void
archiveThread() {
  env::setThreadName("dxmt-pso-archive");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  auto &a = archive();
  char report[2048];
  while (true) {
    {
      std::unique_lock<dxmt::mutex> lock(a.mutex);
      a.cond.wait_for(lock, kFlushPeriod, [&] { return a.requested; });
      a.requested = false;
    }
    report[0] = '\0';
    WMTPSOArchiveFlush(report, sizeof(report));
    logReport(report);
  }
}

std::string
archiveSection() {
  auto &a = archive();
  if (!a.on.load(std::memory_order_acquire))
    return {};
  WMTPSOArchiveStats s = {};
  WMTPSOArchiveQuery(&s);
  auto d = [&](uint64_t now, uint64_t before) { return now >= before ? now - before : now; };
  auto text = std::format(
      " archive={}/{} added={} queued={} unflushed={} addfailed={} capped={}", d(s.lookup_hits, a.last.lookup_hits),
      d(s.lookup_misses, a.last.lookup_misses), d(s.added, a.last.added), s.queued, s.unflushed, s.add_failed,
      s.capped
  );
  a.last = s;
  return text;
}

std::string
section(double, uint32_t) {
  auto x = [](std::atomic<uint64_t> &v) { return v.exchange(0, std::memory_order_relaxed); };
  auto queue_now = std::max<int64_t>(0, g_queue.load(std::memory_order_relaxed));
  auto queue_max = g_queue_max.exchange(queue_now, std::memory_order_relaxed);
  auto &w = g_window;
  return std::format(
      "variants={} air={}/{} pso={}/{} built={} failed={} build=shader:{:.0f}/{:.1f} pipeline:{:.0f}/{:.1f} "
      "queue={}/{} threads={} wait={}x {:.1f}/{:.1f} slow={}{}",
      x(w.variants), x(w.air_hit), x(w.air_miss), x(w.pso_hit), x(w.pso_miss), x(w.built), x(w.failed),
      x(w.shader_ns) / 1e6, x(g_shader_max_ns) / 1e6, x(w.pso_ns) / 1e6, x(g_pso_max_ns) / 1e6, queue_now,
      queue_max, g_threads.load(std::memory_order_relaxed), x(g_wait_n), x(g_wait_ns) / 1e6, x(g_wait_max_ns) / 1e6,
      x(g_slow_frames), archiveSection()
  );
}

SectionRegistrar g_section("COMPILE", section);

} // namespace

void
compileVariantNew() {
  if (statsEnabled())
    bump(&Counts::variants);
}

void
compilePipelineLookup(bool hit) {
  if (statsEnabled())
    bump(hit ? &Counts::pso_hit : &Counts::pso_miss);
}

void
compileAirLookup(bool hit) {
  if (statsEnabled())
    bump(hit ? &Counts::air_hit : &Counts::air_miss);
}

int64_t
compileClock() {
  return statsEnabled() ? nowNs() : 0;
}

void
compileShaderBuilt(int64_t t0) {
  if (!t0)
    return;
  uint64_t ns = nowNs() - t0;
  bump(&Counts::shader_ns, ns);
  atomicMax(g_shader_max_ns, ns);
}

void
compilePipelineBuilt(int64_t t0, bool ok) {
  if (t0) {
    uint64_t ns = nowNs() - t0;
    bump(ok ? &Counts::built : &Counts::failed);
    bump(&Counts::pso_ns, ns);
    atomicMax(g_pso_max_ns, ns);
  }
  auto &a = archive();
  if (ok && a.on.load(std::memory_order_acquire) &&
      (a.built.fetch_add(1, std::memory_order_relaxed) + 1) % compileSettings().flush_every == 0)
    psoArchiveRequestFlush();
}

void
compileQueue(int64_t delta) {
  if (!statsEnabled())
    return;
  auto now = g_queue.fetch_add(delta, std::memory_order_relaxed) + delta;
  if (now > 0)
    atomicMax(g_queue_max, uint64_t(now));
}

void
compileThreads(uint64_t threads) {
  if (statsEnabled())
    atomicMax(g_threads, threads);
}

void
compileEncodeFrame(uint64_t frame) {
  if (!statsEnabled() || frame == t_frame)
    return;
  finishFrame();
  t_frame = frame;
}

CompileWaitScope::CompileWaitScope() {
  if (statsEnabled())
    t0_ = nowNs();
}

CompileWaitScope::~CompileWaitScope() {
  if (!t0_)
    return;
  uint64_t ns = nowNs() - t0_;
  g_wait_n.fetch_add(1, std::memory_order_relaxed);
  g_wait_ns.fetch_add(ns, std::memory_order_relaxed);
  atomicMax(g_wait_max_ns, ns);
  t_wait_n++;
  t_wait_ns += ns;
  t_wait_max_ns = std::max(t_wait_max_ns, ns);
}

const CompileSettings &
compileSettings() {
  static const CompileSettings settings = [] {
    CompileSettings s;
    if (auto v = env::getEnvVar("DXMT_COMPILE_WAIT_MS"); !v.empty())
      s.wait_ms = std::max(0.0, std::atof(v.c_str()));
    s.archive_requested = env::getEnvVar("DXMT_PSO_ARCHIVE") == "1";
    s.archive_dir = env::getEnvVar("DXMT_PSO_ARCHIVE_DIR");
    if (auto v = env::getEnvVar("DXMT_PSO_FLUSH_EVERY"); !v.empty() && std::atoi(v.c_str()) > 0)
      s.flush_every = std::atoi(v.c_str());
    return s;
  }();
  return settings;
}

bool
psoArchiveOn() {
  return archive().on.load(std::memory_order_acquire);
}

void
psoArchiveInit(uint64_t mtl_device) {
  static std::once_flag once;
  std::call_once(once, [mtl_device] {
    auto &settings = compileSettings();
    if (!settings.archive_requested)
      return;
    auto &a = archive();
    auto exe = env::getExeName();
    auto &dir = settings.archive_dir;
    WMTPSOArchiveConfig config = {};
    config.exe_name.set(exe.c_str());
    config.base_dir.set(dir.empty() ? nullptr : dir.c_str());
    config.keep_sessions = kKeepSessions;
    config.max_pipelines = kMaxPipelines;
    std::string report(8192, '\0');
    bool on = WMTPSOArchiveInit(mtl_device, &config, report.data(), report.size());
    logReport(report.c_str());
    if (!on)
      return;
    a.on.store(true, std::memory_order_release);
    Logger::info(str::format(
        "PSO archive: flushing every ", settings.flush_every, " new pipelines, every ",
        std::chrono::duration_cast<std::chrono::seconds>(kFlushPeriod).count(), " s if any are new (not at exit)"
    ));
    dxmt::thread([] { archiveThread(); }).detach();
  });
}

void
psoArchiveRequestFlush() {
  auto &a = archive();
  if (!a.on.load(std::memory_order_acquire))
    return;
  {
    std::lock_guard<dxmt::mutex> lock(a.mutex);
    a.requested = true;
  }
  a.cond.notify_one();
}

} // namespace dxmt::gw2
