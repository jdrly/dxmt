#include "dxmt_gw2_compile.hpp"
#include "dxmt_compile_service.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
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
// gw2 (C2): =1 only (exactly "1"), off by default
sw::Switch<bool> kCompileDemand{"DXMT_COMPILE_DEMAND", false, sw::legacy::exactlyOne};
std::atomic<uint64_t> g_promoted{0};
// gw2 (W0): per variant stage: count, total and max ns in the window
std::atomic<uint64_t> g_stage_n[uint32_t(VariantStage::Count)], g_stage_ns[uint32_t(VariantStage::Count)],
    g_stage_max_ns[uint32_t(VariantStage::Count)];
constexpr const char *kStageNames[] = {"lock", "air", "lib", "fn", "xlate", "store"};
} // namespace

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
std::atomic<uint64_t> g_wait_max_ns, g_slow_frames; // wait count and time: the per-cause counters (#46)

// gw2 (#46): waits by what the pipeline was doing when the wait started
enum WaitCause { CauseQueued, CauseVariantQueued, CauseVariant, CauseBuilding, CauseOther, CauseCount };
constexpr const char *kCauseNames[CauseCount] = {"queued", "variantq", "variant", "building", "other"};
std::atomic<uint64_t> g_cause_n[CauseCount], g_cause_ns[CauseCount], g_archbusy;
std::atomic<int64_t> g_queue;
std::atomic<uint64_t> g_queue_max, g_threads;

// --- the encoding thread's current frame

thread_local uint64_t t_frame = ~0ull;
thread_local uint64_t t_wait_n = 0, t_wait_ns = 0, t_wait_max_ns = 0;
// gw2 (#46): the frame's waits by cause, and its longest wait's timeline (ns; -1 = not known)
thread_local uint64_t t_cause_n[CauseCount];
struct LongestWait {
  int cause = CauseOther;
  int64_t lead = -1, queue = -1, parked = -1, build = -1, varq = -1, varc = -1, depth = 0, archq = -1;
};
thread_local LongestWait t_longest;
thread_local int64_t t_last_line_ns = 0;
thread_local uint64_t t_dropped = 0;

double
waitLineMs() {
  return compileSettings().wait_ms;
}

constexpr int64_t kLineIntervalNs = 200'000'000; // at most 5 COMPILE frame lines per second

// " cause=queued:N,… longest=<cause> lead:… queue:… parked:… build:… varq:… varc:… depth:… archq:…"
// (#46): the frame's waits by cause and the timeline of its longest wait, ms; "-" where unknown
std::string
frameCauses() {
  std::string text = " cause=";
  for (int c = 0; c < CauseCount; c++)
    text += std::format("{}{}:{}", c ? "," : "", kCauseNames[c], t_cause_n[c]);
  auto ms = [](int64_t ns) { return ns < 0 ? std::string("-") : std::format("{:.1f}", ns / 1e6); };
  auto &l = t_longest;
  text += std::format(
      " longest={} lead:{} queue:{} parked:{} build:{} varq:{} varc:{} depth:{} archq:{}", kCauseNames[l.cause],
      ms(l.lead), ms(l.queue), ms(l.parked), ms(l.build), ms(l.varq), ms(l.varc), l.depth,
      l.archq < 0 ? std::string("-") : std::to_string(l.archq)
  );
  return text;
}

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
          "build=shader:{:.0f} pipeline:{:.0f}{}{}",
          t_frame, t_wait_n, ms, t_wait_max_ns / 1e6, std::max<int64_t>(0, g_queue.load(std::memory_order_relaxed)),
          variants, air_hit, air_miss, pso_hit, pso_miss, built, failed, shader_ns / 1e6, pso_ns / 1e6,
          frameCauses(), t_dropped ? std::format(" dropped={}", t_dropped) : std::string()
      ));
      t_last_line_ns = now;
      t_dropped = 0;
    } else {
      t_dropped++;
    }
  }
  t_wait_n = t_wait_ns = t_wait_max_ns = 0;
  for (auto &n : t_cause_n)
    n = 0;
  t_longest = {};
}

// --- PSO archives

constexpr uint32_t kKeepSessions = 4;
constexpr uint32_t kMaxPipelines = 8192;
constexpr auto kFlushPeriod = std::chrono::seconds(30); // gw2 (#52): was 120; a kill loses less

// gw2 (#75): =1 (exactly; off by default), with DXMT_PSO_ARCHIVE=1: the generation publisher and the
// exact-shape failure policy instead of one file per session. Read once, by psoArchiveInit.
sw::Switch<bool> kPsoGenerations{"DXMT_PSO_GENERATIONS", false, sw::legacy::exactlyOne};

struct ArchiveState {
  std::atomic<bool> on = false;
  std::atomic<uint64_t> mode = 0; // gw2 (#75): WMTPSOArchiveStats::mode once the archives are on
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
      " archive={}/{} miss=new:{},known:{} added={} queued={} unflushed={} addfailed={} capped={} skipped={} "
      "rebuilt={}",
      d(s.lookup_hits, a.last.lookup_hits), d(s.lookup_misses, a.last.lookup_misses),
      d(s.miss_new, a.last.miss_new), d(s.miss_known, a.last.miss_known), d(s.added, a.last.added), s.queued,
      s.unflushed, s.add_failed, s.capped, s.skipped, s.rebuilds
  );
  if (s.mode) // gw2 (#75): generations only
    text += std::format(
        " gen={} published={} excluded={} missexcl={}", s.generation, s.published, s.excluded,
        d(s.miss_excluded, a.last.miss_excluded)
    );
  a.last = s;
  return text;
}

// gw2 (#75): the startup line's DXMT_PSO_GENERATIONS: 1 in use, lookup (another process writes the
// store), unused (asked for, but the archives are off or failed), 0 not asked for
sw::Effective kPsoGenerationsLine{"DXMT_PSO_GENERATIONS", [] {
  if (!kPsoGenerations())
    return std::string("0");
  switch (archive().mode.load(std::memory_order_acquire)) {
  case 1:
    return std::string("1");
  case 2:
    return std::string("lookup");
  default:
    return std::string("unused");
  }
}};

// The window's waits, drained once per cause (#46): `wait=` is their sum, so it always equals the
// `waitcause=` breakdown, whatever window a wait finishing during the drain lands in.
struct WaitWindow {
  uint64_t n[CauseCount], ns[CauseCount], total_n = 0, total_ns = 0, archbusy = 0;
};

WaitWindow
drainWaits() {
  WaitWindow w;
  for (int c = 0; c < CauseCount; c++) {
    w.n[c] = g_cause_n[c].exchange(0, std::memory_order_relaxed);
    w.ns[c] = g_cause_ns[c].exchange(0, std::memory_order_relaxed);
    w.total_n += w.n[c];
    w.total_ns += w.ns[c];
  }
  w.archbusy = g_archbusy.exchange(0, std::memory_order_relaxed);
  return w;
}

// " waitcause=queued:N/ms,variantq:…,variant:…,building:…[,other:…] archbusy=N" (#46), window totals;
// empty in a window without waits
std::string
windowCauses(const WaitWindow &w) {
  auto &n = w.n;
  auto &ns = w.ns;
  if (!w.total_n)
    return {};
  std::string text = " waitcause=";
  for (int c = 0; c < CauseCount; c++) {
    if (c == CauseOther && !n[c])
      continue;
    text += std::format("{}{}:{}/{:.1f}", c ? "," : "", kCauseNames[c], n[c], ns[c] / 1e6);
  }
  if (psoArchiveOn())
    text += std::format(" archbusy={}", w.archbusy);
  return text;
}

// " variant=lock:N/ms/max,air:…,lib:…,fn:…,xlate:…,store:…" (W0), window totals; empty in a window
// without variant builds
std::string
variantStages() {
  uint64_t n[uint32_t(VariantStage::Count)], ns[uint32_t(VariantStage::Count)], mx[uint32_t(VariantStage::Count)];
  uint64_t any = 0;
  for (uint32_t i = 0; i < uint32_t(VariantStage::Count); i++) {
    n[i] = g_stage_n[i].exchange(0, std::memory_order_relaxed);
    ns[i] = g_stage_ns[i].exchange(0, std::memory_order_relaxed);
    mx[i] = g_stage_max_ns[i].exchange(0, std::memory_order_relaxed);
    any += n[i];
  }
  if (!any)
    return {};
  std::string text = " variant=";
  for (uint32_t i = 0; i < uint32_t(VariantStage::Count); i++)
    text += std::format("{}{}:{}/{:.1f}/{:.1f}", i ? "," : "", kStageNames[i], n[i], ns[i] / 1e6, mx[i] / 1e6);
  return text;
}

std::string
section(double, uint32_t) {
  auto x = [](std::atomic<uint64_t> &v) { return v.exchange(0, std::memory_order_relaxed); };
  auto queue_now = std::max<int64_t>(0, g_queue.load(std::memory_order_relaxed));
  auto queue_max = g_queue_max.exchange(queue_now, std::memory_order_relaxed);
  auto &w = g_window;
  auto waits = drainWaits();
  return std::format(
      "variants={} air={}/{} pso={}/{} built={} failed={} build=shader:{:.0f}/{:.1f} pipeline:{:.0f}/{:.1f} "
      "queue={}/{} threads={} wait={}x {:.1f}/{:.1f} slow={}{}{}{}",
      x(w.variants), x(w.air_hit), x(w.air_miss), x(w.pso_hit), x(w.pso_miss), x(w.built), x(w.failed),
      x(w.shader_ns) / 1e6, x(g_shader_max_ns) / 1e6, x(w.pso_ns) / 1e6, x(g_pso_max_ns) / 1e6, queue_now,
      queue_max, g_threads.load(std::memory_order_relaxed), waits.total_n, waits.total_ns / 1e6,
      x(g_wait_max_ns) / 1e6, x(g_slow_frames), windowCauses(waits) + variantStages(),
      compileDemand() ? std::format(" promoted={}", x(g_promoted)) : std::string(), archiveSection()
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

// --- gw2 (#46, C1): the compile jobs' timelines are kept by the compile service
// (dxmt_compile_service.hpp); relaxed: each field has one writer at a time, and the encode thread
// reads them only to classify and report, after the job's done flag for the end

void
compileVariantStage(VariantStage stage, int64_t t0) {
  if (!t0)
    return;
  uint64_t ns = uint64_t(std::max<int64_t>(0, nowNs() - t0));
  auto i = uint32_t(stage);
  g_stage_n[i].fetch_add(1, std::memory_order_relaxed);
  g_stage_ns[i].fetch_add(ns, std::memory_order_relaxed);
  atomicMax(g_stage_max_ns[i], ns);
}

bool
compileTiming() {
  return statsEnabled();
}

bool
compileDemand() {
  static const bool on = kCompileDemand();
  return on;
}

void
compilePromoted() {
  if (statsEnabled())
    g_promoted.fetch_add(1, std::memory_order_relaxed);
}

int64_t
compileNowNs() {
  return nowNs();
}

// A job submitted with STATS off has no timeline and isn't classified (as before C1).
static bool
timed(const compile::JobRecord *job) {
  return job && job->timed.load(std::memory_order_relaxed);
}

CompileWaitScope::CompileWaitScope(const compile::JobRecord *job) {
  if (!statsEnabled())
    return;
  t0_ = nowNs();
  job_ = job;
  cause_ = CauseOther;
  if (timed(job)) {
    switch (job->state.load(std::memory_order_acquire)) {
    case compile::JobState::Queued:
      cause_ = CauseQueued;
      break;
    case compile::JobState::Running:
    case compile::JobState::Parked: {
      auto dep = job->parked_on.load(std::memory_order_relaxed);
      cause_ = timed(dep) && dep->state.load(std::memory_order_acquire) == compile::JobState::Queued
                   ? CauseVariantQueued
                   : CauseVariant;
      break;
    }
    case compile::JobState::Building:
      cause_ = CauseBuilding;
      break;
    default:
      break;
    }
  }
  depth_ = std::max<int64_t>(0, g_queue.load(std::memory_order_relaxed));
  if (psoArchiveOn()) {
    // a wait is rare (a few a second at most) and the encode thread is about to block anyway
    WMTPSOArchiveStats s = {};
    WMTPSOArchiveQuery(&s);
    archq_ = int64_t(s.queued);
  }
}

CompileWaitScope::~CompileWaitScope() {
  if (!t0_)
    return;
  uint64_t ns = nowNs() - t0_;
  atomicMax(g_wait_max_ns, ns);
  g_cause_n[cause_].fetch_add(1, std::memory_order_relaxed);
  g_cause_ns[cause_].fetch_add(ns, std::memory_order_relaxed);
  if (archq_ > 0)
    g_archbusy.fetch_add(1, std::memory_order_relaxed);
  t_cause_n[cause_]++;
  if (ns > t_wait_max_ns) {
    // the longest wait of the frame: its pipeline is done now, so its stamps are final
    LongestWait l;
    l.cause = cause_;
    l.depth = depth_;
    l.archq = archq_;
    if (timed(job_)) {
      auto submit = job_->submit_ns.load(std::memory_order_relaxed);
      auto start = job_->start_ns.load(std::memory_order_relaxed);
      auto build = job_->build_ns.load(std::memory_order_relaxed);
      auto done = job_->done_ns.load(std::memory_order_relaxed);
      if (submit) {
        l.lead = t0_ - submit;
        if (start)
          l.queue = start - submit;
      }
      l.parked = job_->parked_ns.load(std::memory_order_relaxed);
      if (build && done >= build)
        l.build = done - build;
      if (auto dep = job_->parked_on.load(std::memory_order_relaxed); timed(dep)) {
        auto dsub = dep->submit_ns.load(std::memory_order_relaxed);
        auto dstart = dep->start_ns.load(std::memory_order_relaxed);
        auto ddone = dep->done_ns.load(std::memory_order_relaxed);
        if (dsub && dstart)
          l.varq = dstart - dsub;
        if (dstart && ddone >= dstart)
          l.varc = ddone - dstart;
      }
    }
    t_longest = l;
  }
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
    config.generations = kPsoGenerations() ? 1 : 0; // gw2 (#75)
    std::string report(8192, '\0');
    bool on = WMTPSOArchiveInit(mtl_device, &config, report.data(), report.size());
    logReport(report.c_str());
    if (!on)
      return;
    WMTPSOArchiveStats s = {}; // gw2 (#75): which mode the unix side runs
    WMTPSOArchiveQuery(&s);
    a.mode.store(s.mode, std::memory_order_release);
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
