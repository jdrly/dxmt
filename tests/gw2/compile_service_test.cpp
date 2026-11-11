// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O1 -Itests/gw2/shim -Isrc/dxmt -Isrc/util tests/gw2/compile_service_test.cpp -o $OUT
//
// gw2 (C1, architecture council §1): native test of the compile service (src/dxmt/dxmt_compile_service.hpp)
// with fake jobs and std:: threads (shim/thread.hpp). No Metal, no Wine.
//
// The legacy parity fixture runs scripted job graphs against the service and against the scheduler it
// replaces (tests/gw2/fixtures/legacy_task_scheduler.hpp, 02532d0's dxmt_tasks.hpp unchanged) and
// requires the same trace from both. A gate holds every job a worker runs. At each step the test waits
// until the system is quiescent (every worker gated or idle) and records:
// - the set of gated jobs (which jobs the queues hand out, continuations first);
// - the worker count (the growth trigger);
// - then it releases the lowest-numbered gated job and records what it returned (done, failed, or
//   parked on a dependency, including the spurious-dependency rerun).
// Wakeups show as the released jobs in the following steps' gated sets. The scenarios include
// GW2-shaped bursts: pipelines that share shader variants, arriving faster than the workers.
//
// Also: exactly one completion per job, the compile queue's +1/-1 balance, the job records' states
// with STATS off (no timestamps), and demands classified by the job's state when they begin.
//
// Run: tests/gw2/run.zsh pure, or the command above with $OUT set.

#include "dxmt_compile_service.hpp"
#include "fixtures/legacy_task_scheduler.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace dxmt;
using Clock = std::chrono::steady_clock;

// --- the telemetry the service calls (production: dxmt_gw2_compile.cpp)
namespace {
std::atomic<bool> g_timing{false};
std::atomic<int64_t> g_queue{0};
std::atomic<int64_t> g_now{1};
std::mutex g_threads_mutex;
std::vector<uint64_t> g_threads; // every compileThreads value, in order
struct WaitRecord {
  bool timed;
  compile::JobState state;
};
std::mutex g_waits_mutex;
std::vector<WaitRecord> g_waits;
// gw2 (#152): every compileWorkerPriority call: its thread, the mode, whether that thread had run a job
struct PriorityCall {
  std::thread::id thread;
  gw2::ThreadPriority mode;
  bool after_a_job;
};
std::mutex g_priority_mutex;
std::vector<PriorityCall> g_priority_calls;
thread_local bool t_ran_a_job = false;
} // namespace

namespace dxmt::gw2 {
void
compileQueue(int64_t delta) {
  g_queue += delta;
}
void
compileThreads(uint64_t threads) {
  std::lock_guard<std::mutex> l(g_threads_mutex);
  g_threads.push_back(threads);
}
bool
compileTiming() {
  return g_timing.load();
}
int64_t
compileNowNs() {
  return g_now.fetch_add(1);
}
CompileWaitScope::CompileWaitScope(const compile::JobRecord *job) {
  std::lock_guard<std::mutex> l(g_waits_mutex);
  g_waits.push_back({job && job->timed.load(), job ? job->state.load() : compile::JobState::Absent});
}
CompileWaitScope::~CompileWaitScope() {}
std::atomic<uint64_t> g_promoted_count{0};
bool
compileDemand() {
  return false;
}
void
compilePromoted() {
  g_promoted_count++;
}
void
compileWorkerPriority(ThreadPriority mode) {
  std::lock_guard<std::mutex> l(g_priority_mutex);
  g_priority_calls.push_back({std::this_thread::get_id(), mode, t_ran_a_job});
}
// #159: the most speculative jobs running at once (production: WARM's jobs=)
std::atomic<uint32_t> g_speculative_peak{0};
void
warmJobsRunning(uint32_t running) {
  uint32_t peak = g_speculative_peak.load();
  while (running > peak && !g_speculative_peak.compare_exchange_weak(peak, running))
    ;
}
} // namespace dxmt::gw2

static int failures = 0;
#define CHECK(cond, ...)                                                                                          \
  do {                                                                                                            \
    if (!(cond)) {                                                                                                \
      failures++;                                                                                                 \
      std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                 \
      std::printf(__VA_ARGS__);                                                                                   \
      std::printf("\n");                                                                                          \
    }                                                                                                             \
  } while (0)

// --- fake jobs and the gate

struct Gate;

struct FakeJob {
  int id;
  std::vector<FakeJob *> deps; // probed in this order, as the pipeline jobs probe their shaders
  bool fail_early = false;     // geometry/tessellation: a done but failed dependency fails the job at once
  bool fails = false;          // its own build fails (done, with no result)
  int lie_once = -1;           // returns deps[lie_once] once although it is done (spurious dependency)
  int64_t cost = 0;            // #159: added to the fake clock (compileNowNs) while it runs: its build time
  Gate *gate = nullptr;
  std::atomic_bool ready{false};
  std::atomic<int> done_count{0};
  bool failed = false;
  compile::JobRecord record;

  FakeJob *run();
  bool GetIsDone() { return ready.load(); }
  void SetIsDone(bool v) {
    done_count++;
    ready.store(v);
    ready.notify_all();
  }
};

struct Gate {
  std::mutex m;
  std::condition_variable cv;
  std::set<int> gated;
  std::set<int> released;
  uint64_t events = 0; // bumps on every enter and return
  std::vector<std::string> outcomes;

  void
  enter(int id) {
    std::unique_lock<std::mutex> l(m);
    gated.insert(id);
    events++;
    cv.notify_all();
    cv.wait(l, [&] { return released.count(id) > 0; });
    released.erase(id);
    gated.erase(id);
  }
  void
  returned(std::string what) {
    std::lock_guard<std::mutex> l(m);
    outcomes.push_back(std::move(what));
    events++;
    cv.notify_all();
  }
};

std::atomic<uint64_t> g_parks{0}, g_early_runs{0};

FakeJob *
FakeJob::run() {
  t_ran_a_job = true;
  if (!gate) {
    // ungated (stress): a declared job must find every dependency done when it starts
    if (std::any_of(deps.begin(), deps.end(), [](FakeJob *d) { return !d->GetIsDone(); }))
      g_early_runs++;
    for (auto *d : deps)
      if (!d->GetIsDone()) {
        g_parks++;
        return d;
      }
    for (volatile int spin = 0; spin < 200 + id % 300; spin = spin + 1)
      ;
    failed = fails;
    return this;
  }
  gate->enter(id);
  if (cost)
    g_now += cost;
  for (size_t i = 0; i < deps.size(); i++) {
    auto *d = deps[i];
    if (int(i) == lie_once) {
      lie_once = -1;
      gate->returned(std::to_string(id) + "->park" + std::to_string(d->id) + "(spurious)");
      return d;
    }
    if (!d->GetIsDone()) {
      gate->returned(std::to_string(id) + "->park" + std::to_string(d->id));
      return d;
    }
    if (fail_early && d->failed) {
      failed = true;
      gate->returned(std::to_string(id) + "->failed(dep" + std::to_string(d->id) + ")");
      return this;
    }
  }
  failed = fails;
  gate->returned(std::to_string(id) + (fails ? "->failed" : "->done"));
  return this;
}

// the legacy scheduler's trait and the service's
namespace dxmt {
template <> struct task_trait<FakeJob *> {
  FakeJob *run_task(FakeJob *job) { return job->run(); }
  bool get_done(FakeJob *job) { return job->GetIsDone(); }
  void set_done(FakeJob *job) { job->SetIsDone(true); }
  void on_submit(FakeJob *) {}
};
template <> struct compile::JobTraits<FakeJob *> {
  FakeJob *run(FakeJob *job) { return job->run(); }
  bool done(FakeJob *job) { return job->GetIsDone(); }
  void setDone(FakeJob *job) { job->SetIsDone(true); }
  compile::JobRecord &record(FakeJob *job) { return job->record; }
  void deps(FakeJob *job, std::vector<FakeJob *> &out) { out.insert(out.end(), job->deps.begin(), job->deps.end()); }
};
} // namespace dxmt

// --- scenarios: jobs and when they are submitted

struct Scenario {
  std::string name;
  std::vector<std::unique_ptr<FakeJob>> jobs;
  std::map<int, std::vector<int>> submit_at; // step -> job ids submitted before it (in this order)

  FakeJob *
  add(std::vector<int> deps = {}) {
    auto j = std::make_unique<FakeJob>();
    j->id = int(jobs.size());
    for (int d : deps)
      j->deps.push_back(jobs[d].get());
    jobs.push_back(std::move(j));
    return jobs.back().get();
  }
};

using ScenarioFn = std::function<void(Scenario &)>;

struct Trace {
  std::vector<std::string> steps;
  std::vector<uint64_t> threads;
  int64_t queue_balance = 0;
  bool exactly_once = true;
  bool stuck = false;
};

// Quiescent: every worker is gated, or nothing has changed for `settle`.
static bool
waitQuiescent(Gate &g, size_t workers, std::chrono::milliseconds settle, std::chrono::seconds limit) {
  auto end = Clock::now() + limit;
  uint64_t last = ~0ull;
  auto stable_since = Clock::now();
  while (Clock::now() < end) {
    {
      std::lock_guard<std::mutex> l(g.m);
      if (g.gated.size() >= workers)
        return true;
      if (g.events != last) {
        last = g.events;
        stable_since = Clock::now();
      }
    }
    if (Clock::now() - stable_since >= settle)
      return true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return false;
}

static size_t
currentWorkers() {
  std::lock_guard<std::mutex> l(g_threads_mutex);
  return g_threads.empty() ? 0 : g_threads.back();
}

template <typename Scheduler>
static Trace
runScenario(const ScenarioFn &build) {
  Scenario sc;
  build(sc);
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  {
    std::lock_guard<std::mutex> l(g_threads_mutex);
    g_threads.clear();
  }
  g_queue = 0;
  Trace t;
  {
    Scheduler sched;
    auto settle = std::chrono::milliseconds(8);
    size_t total = sc.jobs.size();
    for (int step = 0; step < 10000; step++) {
      // one submit at a time, each settled: the growth trigger reads the running count, which a
      // worker raises after it pops, so submits racing the pops would make growth timing-dependent
      // (in both schedulers alike)
      if (auto it = sc.submit_at.find(step); it != sc.submit_at.end())
        for (int id : it->second) {
          sched.submit(sc.jobs[id].get());
          if (!waitQuiescent(gate, currentWorkers(), settle, std::chrono::seconds(10)))
            t.stuck = true;
        }
      if (!waitQuiescent(gate, currentWorkers(), settle, std::chrono::seconds(10))) {
        t.stuck = true;
        break;
      }
      std::set<int> gated;
      {
        std::lock_guard<std::mutex> l(gate.m);
        gated = gate.gated;
      }
      bool all_done = std::all_of(sc.jobs.begin(), sc.jobs.end(), [](auto &j) { return j->GetIsDone(); });
      bool more_submits = sc.submit_at.upper_bound(step) != sc.submit_at.end();
      if (gated.empty()) {
        if (all_done && !more_submits)
          break;
        if (!more_submits) {
          t.stuck = true; // nothing runs, nothing to submit, jobs not done
          break;
        }
        t.steps.push_back("idle");
        continue;
      }
      std::ostringstream s;
      s << "gated{";
      for (int id : gated)
        s << id << ",";
      s << "} workers=" << currentWorkers();
      int pick = *gated.begin();
      size_t before;
      {
        std::lock_guard<std::mutex> l(gate.m);
        before = gate.outcomes.size();
        gate.released.insert(pick);
        gate.cv.notify_all();
      }
      // wait for its outcome
      auto end = Clock::now() + std::chrono::seconds(10);
      std::string outcome;
      while (Clock::now() < end) {
        std::lock_guard<std::mutex> l(gate.m);
        if (gate.outcomes.size() > before) {
          outcome = gate.outcomes[before];
          break;
        }
      }
      s << " run " << pick << " => " << outcome;
      t.steps.push_back(s.str());
      (void)total;
    }
    // let the last completion settle before stopping
    waitQuiescent(gate, currentWorkers(), settle, std::chrono::seconds(10));
  }
  {
    std::lock_guard<std::mutex> l(g_threads_mutex);
    t.threads = g_threads;
  }
  t.queue_balance = g_queue.load();
  for (auto &j : sc.jobs)
    if (j->done_count.load() != 1)
      t.exactly_once = false;
  return t;
}

static void
parity(const std::string &name, const ScenarioFn &build) {
  auto legacy = runScenario<task_scheduler<FakeJob *>>(build);
  auto service = runScenario<compile::Service<FakeJob *>>(build);
  CHECK(!legacy.stuck, "%s: the legacy run got stuck", name.c_str());
  CHECK(!service.stuck, "%s: the service run got stuck", name.c_str());
  CHECK(legacy.exactly_once && service.exactly_once, "%s: every job completes exactly once", name.c_str());
  CHECK(legacy.queue_balance == 0 && service.queue_balance == 0, "%s: queue balance %lld/%lld", name.c_str(),
        (long long)legacy.queue_balance, (long long)service.queue_balance);
  CHECK(legacy.threads == service.threads, "%s: worker growth differs (%zu vs %zu changes)", name.c_str(),
        legacy.threads.size(), service.threads.size());
  bool same = legacy.steps == service.steps;
  CHECK(same, "%s: traces differ", name.c_str());
  if (!same) {
    size_t n = std::max(legacy.steps.size(), service.steps.size());
    for (size_t i = 0; i < n; i++) {
      auto a = i < legacy.steps.size() ? legacy.steps[i] : "-";
      auto b = i < service.steps.size() ? service.steps[i] : "-";
      if (a != b) {
        std::printf("  step %zu\n    legacy:  %s\n    service: %s\n", i, a.c_str(), b.c_str());
        break;
      }
    }
  }
  std::printf("%s %s: %zu steps, workers %llu\n", same && !legacy.stuck ? "ok  " : "FAIL", name.c_str(),
              service.steps.size(), (unsigned long long)(service.threads.empty() ? 0 : service.threads.back()));
}

// --- scenarios

// A pipeline's creation, as GW2 does it: get_shader submits each new variant, then the pipeline.
static void
pipeline(Scenario &sc, int step, std::vector<int> new_variants, std::vector<int> deps, bool fail_early = false) {
  auto *p = sc.add(deps);
  p->fail_early = fail_early;
  for (int v : new_variants)
    sc.submit_at[step].push_back(v);
  sc.submit_at[step].push_back(p->id);
}

static void
chain(Scenario &sc) {
  sc.add(); // 0: VS
  sc.add(); // 1: PS
  pipeline(sc, 0, {0, 1}, {0, 1});
}

static void
sharedVariants(Scenario &sc) {
  for (int i = 0; i < 4; i++)
    sc.add(); // 0-3: two VS, two PS
  pipeline(sc, 0, {0, 2}, {0, 2});
  pipeline(sc, 0, {1}, {1, 2});
  pipeline(sc, 0, {3}, {0, 3});
  pipeline(sc, 0, {}, {1, 3});
}

static void
continuationsFirst(Scenario &sc) {
  sc.add(); // 0: a variant
  pipeline(sc, 0, {0}, {0});
  pipeline(sc, 0, {}, {0});
  for (int i = 0; i < 4; i++) {
    auto *x = sc.add(); // independent jobs queued behind
    sc.submit_at[0].push_back(x->id);
  }
}

// Parked jobs released while independent jobs wait in the FIFO: the released ones run first. Queues
// build only when every worker is busy at the limit (2 x CPUs), so fillers saturate the workers first;
// the pipelines are submitted before their variant, so they park on it.
static void
parkedBeforeFifo(Scenario &sc) {
  sc.add();              // 0: a variant
  auto *a = sc.add({0}); // 1, 2: pipelines on it
  auto *b = sc.add({0});
  int limit = int(std::thread::hardware_concurrency()) * 2;
  for (int i = 0; i < limit; i++)
    sc.submit_at[0].push_back(sc.add()->id); // fillers: every worker busy
  for (int id : {a->id, b->id, 0})
    sc.submit_at[0].push_back(id);
  for (int i = 0; i < 6; i++)
    sc.submit_at[0].push_back(sc.add()->id); // independent jobs queued behind them
}

static void
failures_(Scenario &sc) {
  auto *v = sc.add();
  v->fails = true; // 0: a variant whose translation fails
  sc.add();        // 1: a geometry shader variant
  sc.add();        // 2: PS
  pipeline(sc, 0, {0, 1, 2}, {0, 1, 2}, true); // geometry pipeline: fails at once on 0
  pipeline(sc, 0, {}, {0, 2});                 // plain pipeline: probes on, builds (with a null function)
}

static void
spurious(Scenario &sc) {
  sc.add();
  sc.add();
  sc.submit_at[0] = {0, 1};
  auto *p = sc.add({0, 1});
  p->lie_once = 0; // probes 0 as not done although it is
  sc.submit_at[3].push_back(p->id);
}

static void
lateSubmits(Scenario &sc) {
  for (int i = 0; i < 3; i++)
    sc.add();
  pipeline(sc, 0, {0, 1}, {0, 1});
  pipeline(sc, 2, {2}, {2, 0});
  pipeline(sc, 4, {}, {1, 2});
  pipeline(sc, 9, {}, {0, 1});
}

// GW2 at a map load: bursts of pipelines faster than the workers. 12 VS and 20 PS variants, 60
// pipelines combining them, in three bursts; later pipelines mostly reuse variants.
static void
burst(Scenario &sc) {
  std::vector<int> vs, ps;
  std::set<int> submitted;
  for (int i = 0; i < 12; i++)
    vs.push_back(sc.add()->id);
  for (int i = 0; i < 20; i++)
    ps.push_back(sc.add()->id);
  int k = 0;
  for (int b = 0; b < 3; b++) {
    int step = b * 25;
    for (int i = 0; i < 20; i++, k++) {
      int v = vs[(k * 7) % vs.size()], p = ps[(k * 11 + b) % ps.size()];
      std::vector<int> fresh;
      for (int x : {v, p})
        if (submitted.insert(x).second)
          fresh.push_back(x);
      pipeline(sc, step, fresh, {v, p});
    }
  }
}

// Wide: many jobs submitted at once (workers grow only while every worker is running).
static void
wide(Scenario &sc) {
  int n = int(std::thread::hardware_concurrency()) * 2 + 6;
  for (int i = 0; i < n; i++)
    sc.submit_at[0].push_back(sc.add()->id);
  pipeline(sc, 1, {}, {0, 1});
}

// Growth: two jobs a step while every worker is busy, past 2 x CPUs, so the trigger fires on most
// submits and the worker count reaches its limit.
static void
growth(Scenario &sc) {
  int limit = int(std::thread::hardware_concurrency()) * 2;
  for (int step = 0; step < limit + 4; step++)
    for (int i = 0; i < 2; i++)
      sc.submit_at[step].push_back(sc.add()->id);
}

// --- record states and demands (the service alone)

static void
recordsAndDemands() {
  g_timing = false;
  Scenario sc;
  sc.add();
  sc.add({0});
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  auto *v = sc.jobs[0].get(), *p = sc.jobs[1].get();
  {
    // one worker: the pipeline runs first and parks on its variant, as when its variant is new
    compile::Service<FakeJob *> sched(compile::Options{1, 1});
    sched.submit(p);
    sched.submit(v);
    auto quiet = [&] { waitQuiescent(gate, 1, std::chrono::milliseconds(8), std::chrono::seconds(5)); };
    auto release = [&](int id) { // and wait for what it returns
      size_t before;
      {
        std::lock_guard<std::mutex> l(gate.m);
        before = gate.outcomes.size();
        gate.released.insert(id);
        gate.cv.notify_all();
      }
      while (true) {
        std::lock_guard<std::mutex> l(gate.m);
        if (gate.outcomes.size() > before)
          break;
      }
    };
    auto waits = [] {
      std::lock_guard<std::mutex> l(g_waits_mutex);
      return g_waits.size();
    };
    quiet();
    CHECK(p->record.state.load() == compile::JobState::Running, "the pipeline runs (STATS off)");
    CHECK(v->record.state.load() == compile::JobState::Queued, "the variant is queued (STATS off)");
    CHECK(!v->record.timed.load() && v->record.submit_ns.load() == 0, "no timeline with STATS off");
    std::thread demand_v([&] { compile::demand(v->record, v->ready); });
    while (waits() < 1)
      std::this_thread::yield();
    release(p->id); // it parks on the variant; the worker takes the variant
    quiet();
    CHECK(p->record.state.load() == compile::JobState::Parked && p->record.parked_on.load() == &v->record,
          "the pipeline is parked on its variant");
    CHECK(v->record.state.load() == compile::JobState::Running, "the variant runs");
    std::thread demand_p([&] { compile::demand(p->record, p->ready); });
    while (waits() < 2)
      std::this_thread::yield();
    release(v->id); // done: the pipeline is a continuation
    quiet();
    release(p->id);
    demand_v.join();
    demand_p.join();
    quiet();
    {
      std::lock_guard<std::mutex> l(g_waits_mutex);
      CHECK(g_waits.size() == 2 && g_waits[0].state == compile::JobState::Queued &&
                g_waits[1].state == compile::JobState::Parked,
            "the demands saw the job's state when they began (queued, parked)");
    }
    CHECK(p->record.state.load() == compile::JobState::Complete && v->record.state.load() == compile::JobState::Complete,
          "both complete");
    compile::demand(p->record, p->ready); // done: no wait
    CHECK(waits() == 2, "no wait recorded for a done job");
  }
  // with STATS on: timestamps in order
  g_timing = true;
  Scenario sc2;
  sc2.add();
  Gate gate2;
  sc2.jobs[0]->gate = &gate2;
  {
    compile::Service<FakeJob *> sched(compile::Options{1, 1});
    sched.submit(sc2.jobs[0].get());
    waitQuiescent(gate2, 1, std::chrono::milliseconds(8), std::chrono::seconds(5));
    {
      std::lock_guard<std::mutex> l(gate2.m);
      gate2.released.insert(0);
      gate2.cv.notify_all();
    }
    sc2.jobs[0]->ready.wait(false);
  }
  auto &r = sc2.jobs[0]->record;
  CHECK(r.timed.load() && r.submit_ns.load() && r.submit_ns.load() < r.start_ns.load() &&
            r.start_ns.load() < r.done_ns.load(),
        "STATS on: submit < start < done");
  g_timing = false;
}

// --- C2: the declared policy (the service alone)

static compile::Options
declared(unsigned workers, unsigned max) {
  compile::Options o;
  o.initial_workers = workers;
  o.max_workers = max;
  o.policy = compile::Policy::Declared;
  return o;
}

// Releases `id` at the gate and waits for what it returns.
static std::string
releaseJob(Gate &gate, int id) {
  size_t before;
  {
    std::lock_guard<std::mutex> l(gate.m);
    before = gate.outcomes.size();
    gate.released.insert(id);
    gate.cv.notify_all();
  }
  auto end = Clock::now() + std::chrono::seconds(10);
  while (Clock::now() < end) {
    std::lock_guard<std::mutex> l(gate.m);
    if (gate.outcomes.size() > before)
      return gate.outcomes[before];
  }
  return "timeout";
}

static std::set<int>
gatedNow(Gate &gate, size_t workers) {
  waitQuiescent(gate, workers, std::chrono::milliseconds(8), std::chrono::seconds(10));
  std::lock_guard<std::mutex> l(gate.m);
  return gate.gated;
}

static std::string
str(const std::set<int> &s) {
  std::string r = "{";
  for (int i : s)
    r += std::to_string(i) + ",";
  return r + "}";
}

// Many threads, GW2-shaped graphs, no gate: every job completes once, none runs before its
// dependencies are done, none parks.
static void
declaredStress() {
  g_parks = 0;
  g_early_runs = 0;
  g_queue = 0;
  constexpr int kThreads = 4, kOwnVariants = 75, kPipelines = 400, kShared = 40;
  Scenario sc;
  for (int i = 0; i < kShared; i++)
    sc.add();
  std::vector<std::vector<FakeJob *>> own(kThreads), pipes(kThreads);
  std::mt19937 rng(42);
  for (int t = 0; t < kThreads; t++) {
    for (int i = 0; i < kOwnVariants; i++)
      own[t].push_back(sc.add());
    for (int i = 0; i < kPipelines; i++) {
      auto *p = sc.add();
      p->deps.push_back(own[t][rng() % kOwnVariants]);
      p->deps.push_back(sc.jobs[rng() % kShared].get());
      if (i % 3 == 0)
        p->deps.push_back(own[t][rng() % kOwnVariants]);
      pipes[t].push_back(p);
    }
  }
  {
    compile::Service<FakeJob *> sched(declared(2, 0));
    for (int i = 0; i < kShared; i++)
      sched.submit(sc.jobs[i].get());
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; t++)
      ts.emplace_back([&, t] {
        // as a pipeline's constructor does through get_shader: its new variants first, then it
        std::set<FakeJob *> submitted;
        for (int i = 0; i < kPipelines; i++) {
          for (auto *d : pipes[t][i]->deps)
            if (d->id >= kShared && submitted.insert(d).second)
              sched.submit(d);
          sched.submit(pipes[t][i]);
        }
        for (auto *v : own[t])
          if (submitted.insert(v).second)
            sched.submit(v);
      });
    for (auto &t : ts)
      t.join();
    for (auto &j : sc.jobs)
      j->ready.wait(false);
  }
  int once = 0;
  for (auto &j : sc.jobs)
    once += j->done_count.load() == 1;
  CHECK(once == int(sc.jobs.size()), "declared stress: %d of %zu jobs completed exactly once", once, sc.jobs.size());
  CHECK(g_queue.load() == 0, "declared stress: queue balance %lld", (long long)g_queue.load());
  CHECK(g_early_runs.load() == 0 && g_parks.load() == 0, "declared stress: %llu jobs ran before a dependency was done, %llu parks",
        (unsigned long long)g_early_runs.load(), (unsigned long long)g_parks.load());
  std::printf("%s declared stress: %zu jobs from %d threads, each once; none ran early or parked\n",
              g_early_runs.load() || g_parks.load() ? "FAIL" : "ok  ", sc.jobs.size(), kThreads);
}

// No parks when dependencies come first: a pipeline submitted while its variants run waits unqueued.
static void
declaredNoPark() {
  Scenario sc;
  sc.add();
  sc.add();
  auto *p = sc.add({0, 1});
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  compile::Service<FakeJob *> sched(declared(3, 3));
  sched.submit(sc.jobs[0].get());
  sched.submit(sc.jobs[1].get());
  sched.submit(p);
  auto g = gatedNow(gate, 3);
  CHECK(g == std::set<int>({0, 1}), "declared: the pipeline waits for its variants, unqueued (gated %s)", str(g).c_str());
  CHECK(p->record.state.load() == compile::JobState::Queued, "declared: the waiting pipeline is still 'queued'");
  auto a = releaseJob(gate, 0), b = releaseJob(gate, 1);
  g = gatedNow(gate, 3);
  CHECK(g == std::set<int>({2}), "declared: runnable once both are done (gated %s)", str(g).c_str());
  auto c = releaseJob(gate, 2);
  CHECK(c == "2->done", "declared: it ran once, without parking (%s)", c.c_str());
  std::printf("%s declared, no park: %s %s %s\n", c == "2->done" ? "ok  " : "FAIL", a.c_str(), b.c_str(), c.c_str());
}

// A demand promotes the job and its queued dependency closure ahead of the FIFO, once.
static void
declaredDemand() {
  gw2::g_promoted_count = 0;
  Scenario sc;
  for (int i = 0; i < 4; i++)
    sc.add();                  // 0-3: independent jobs
  sc.add();                    // 4: a variant
  auto *p = sc.add({4});       // 5: the pipeline on it
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  std::vector<std::string> order;
  {
    compile::Service<FakeJob *> sched(declared(1, 1));
    for (int i = 0; i < 6; i++)
      sched.submit(sc.jobs[i].get());
    auto g = gatedNow(gate, 1);
    CHECK(g == std::set<int>({0}), "demand: job 0 runs (gated %s)", str(g).c_str());
    std::thread demander([&] { compile::demand(p->record, p->ready); });
    while (gw2::g_promoted_count.load() < 2)
      std::this_thread::yield();
    // twice: promotion happens once per job
    std::thread demander2([&] { compile::demand(p->record, p->ready); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(gw2::g_promoted_count.load() == 2, "demand: the pipeline and its variant promoted once each (%llu)",
          (unsigned long long)gw2::g_promoted_count.load());
    for (int step = 0; step < 6; step++) {
      auto gs = gatedNow(gate, 1);
      if (gs.empty())
        break;
      order.push_back(releaseJob(gate, *gs.begin()));
    }
    demander.join();
    demander2.join();
  }
  std::string got;
  for (auto &o : order)
    got += o + " ";
  bool ok = got == "0->done 4->done 5->done 1->done 2->done 3->done ";
  CHECK(ok, "demand: the variant and the pipeline run before the FIFO (%s)", got.c_str());
  std::printf("%s declared, demand promotes the closure: %s\n", ok ? "ok  " : "FAIL", got.c_str());
}

// Speculative jobs: at most one at a time, none while a demand waits; a demand on one promotes it.
static void
declaredSpeculative() {
  Scenario sc;
  for (int i = 0; i < 3; i++)
    sc.add(); // 0-2: speculative (warming)
  sc.add();   // 3: normal
  sc.add();   // 4: speculative, demanded later
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  compile::Service<FakeJob *> sched(declared(3, 3));
  for (int i = 0; i < 3; i++)
    sched.submit(sc.jobs[i].get(), compile::Lane::Speculative);
  sched.submit(sc.jobs[3].get());
  auto g = gatedNow(gate, 3);
  CHECK(g == std::set<int>({0, 3}), "speculative: one speculative job beside the normal one (gated %s)", str(g).c_str());
  // a demand pending (on the running normal job): no speculative job starts
  auto *n = sc.jobs[3].get();
  std::thread demander([&] { compile::demand(n->record, n->ready); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  releaseJob(gate, 0);
  g = gatedNow(gate, 3);
  CHECK(g == std::set<int>({3}), "speculative: none starts while a demand waits (gated %s)", str(g).c_str());
  releaseJob(gate, 3);
  demander.join();
  g = gatedNow(gate, 3);
  CHECK(g == std::set<int>({1}), "speculative: admission resumes after the demand (gated %s)", str(g).c_str());
  // a demand on a queued speculative job promotes it past the speculative limit
  auto *s4 = sc.jobs[4].get();
  sched.submit(s4, compile::Lane::Speculative);
  std::thread demander4([&] { compile::demand(s4->record, s4->ready); });
  g = gatedNow(gate, 3);
  for (int tries = 0; tries < 100 && g.size() < 2; tries++)
    g = gatedNow(gate, 3);
  CHECK(g == std::set<int>({1, 4}), "speculative: a demanded speculative job runs at once (gated %s)", str(g).c_str());
  releaseJob(gate, 4);
  demander4.join();
  releaseJob(gate, 1);
  g = gatedNow(gate, 3);
  releaseJob(gate, 2);
  std::printf("%s declared, speculative admission\n", failures ? "?   " : "ok  ");
}

// A dependency the service doesn't know (submitted after its dependent): the job parks on it as
// legacy does and completes after it.
static void
declaredUnknownDependency() {
  Scenario sc;
  sc.add();
  auto *p = sc.add({0});
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  compile::Service<FakeJob *> sched(declared(1, 1));
  sched.submit(p);
  gatedNow(gate, 1);
  auto a = releaseJob(gate, 1);
  sched.submit(sc.jobs[0].get());
  gatedNow(gate, 1);
  auto b = releaseJob(gate, 0);
  gatedNow(gate, 1);
  auto c = releaseJob(gate, 1);
  bool ok = a == "1->park0" && b == "0->done" && c == "1->done";
  CHECK(ok, "unknown dependency: %s %s %s", a.c_str(), b.c_str(), c.c_str());
  std::printf("%s declared, unknown dependency parks: %s %s %s\n", ok ? "ok  " : "FAIL", a.c_str(), b.c_str(), c.c_str());
}

// A check whose failure would leave a demand waiting forever: report and end the process.
#define CHECK_OR_EXIT(cond, ...)                                                                                  \
  do {                                                                                                            \
    if (!(cond)) {                                                                                                \
      std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                 \
      std::printf(__VA_ARGS__);                                                                                   \
      std::printf("\n1 FAILED (a demand would wait forever)\n");                                                  \
      std::fflush(stdout);                                                                                        \
      _exit(1);                                                                                                   \
    }                                                                                                             \
  } while (0)

// #74 review, Blocker: a normal job parks on a dependency not submitted yet; a demand on the job;
// then the dependency is submitted as speculative. The demand must reach it (else speculative
// admission refuses it while the demand waits, and the demand waits for it: forever).
static void
declaredLateSpeculativeDependency() {
  Scenario sc;
  sc.add();               // 0: D, submitted late, speculative
  auto *p = sc.add({0});  // 1: P, normal
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  auto *d = sc.jobs[0].get();
  compile::Service<FakeJob *> sched(declared(2, 2));
  sched.submit(p);
  gatedNow(gate, 2);
  auto a = releaseJob(gate, 1); // P parks on the unknown D
  gw2::g_promoted_count = 0;
  std::thread demander([&] { compile::demand(p->record, p->ready); });
  while (gw2::g_promoted_count.load() < 1) // the demand has promoted P (and registered D)
    std::this_thread::yield();
  sched.submit(d, compile::Lane::Speculative);
  auto g = gatedNow(gate, 2);
  CHECK_OR_EXIT(g == std::set<int>({0}), "late dependency: the demand reaches the speculative dependency (gated %s)",
                str(g).c_str());
  auto b = releaseJob(gate, 0);
  g = gatedNow(gate, 2);
  CHECK_OR_EXIT(g == std::set<int>({1}), "late dependency: the demanded job resumes (gated %s)", str(g).c_str());
  auto c = releaseJob(gate, 1);
  demander.join();
  bool ok = a == "1->park0" && b == "0->done" && c == "1->done";
  CHECK(ok, "late dependency: %s %s %s", a.c_str(), b.c_str(), c.c_str());
  std::printf("%s declared, demand reaches a late speculative dependency: %s %s %s\n", ok ? "ok  " : "FAIL", a.c_str(),
              b.c_str(), c.c_str());
}

// #74 review, High: a speculative job parked on an unknown dependency resumes through speculative
// admission: not while a demand waits, not beside another speculative job.
static void
declaredSpeculativeResume() {
  {
    // (a) a demand pending on an unrelated job
    Scenario sc;
    sc.add();              // 0: U, unknown at first, normal
    auto *s1 = sc.add({0}); // 1: S, speculative
    auto *n = sc.add();    // 2: N, normal, demanded
    Gate gate;
    for (auto &j : sc.jobs)
      j->gate = &gate;
    compile::Service<FakeJob *> sched(declared(3, 3));
    sched.submit(n);
    sched.submit(s1, compile::Lane::Speculative);
    auto g = gatedNow(gate, 3);
    CHECK(g == std::set<int>({1, 2}), "resume (a): N and S run (gated %s)", str(g).c_str());
    auto a = releaseJob(gate, 1); // S parks on U
    std::thread demander([&] { compile::demand(n->record, n->ready); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sched.submit(sc.jobs[0].get());
    g = gatedNow(gate, 3);
    CHECK(g == std::set<int>({0, 2}), "resume (a): U runs beside N (gated %s)", str(g).c_str());
    releaseJob(gate, 0); // U done: S is released, but a demand waits
    g = gatedNow(gate, 3);
    CHECK(g == std::set<int>({2}), "resume (a): S doesn't resume while the demand waits (gated %s)", str(g).c_str());
    releaseJob(gate, 2);
    demander.join();
    g = gatedNow(gate, 3);
    CHECK(g == std::set<int>({1}), "resume (a): S resumes after the demand (gated %s)", str(g).c_str());
    auto c = releaseJob(gate, 1);
    CHECK(a == "1->park0" && c == "1->done", "resume (a): %s %s", a.c_str(), c.c_str());
  }
  {
    // (b) another speculative job running
    Scenario sc;
    sc.add();               // 0: U
    auto *s1 = sc.add({0}); // 1: S1, speculative, parks on U
    auto *s2 = sc.add();    // 2: S2, speculative
    Gate gate;
    for (auto &j : sc.jobs)
      j->gate = &gate;
    compile::Service<FakeJob *> sched(declared(3, 3));
    sched.submit(s1, compile::Lane::Speculative);
    gatedNow(gate, 3);
    releaseJob(gate, 1); // S1 parks; its slot is free
    sched.submit(s2, compile::Lane::Speculative);
    auto g = gatedNow(gate, 3);
    CHECK(g == std::set<int>({2}), "resume (b): S2 takes the free slot (gated %s)", str(g).c_str());
    sched.submit(sc.jobs[0].get());
    g = gatedNow(gate, 3);
    CHECK(g == std::set<int>({0, 2}), "resume (b): U runs (gated %s)", str(g).c_str());
    releaseJob(gate, 0); // S1 released while S2 runs
    g = gatedNow(gate, 3);
    CHECK(g == std::set<int>({2}), "resume (b): S1 waits for S2's slot (gated %s)", str(g).c_str());
    releaseJob(gate, 2);
    g = gatedNow(gate, 3);
    CHECK(g == std::set<int>({1}), "resume (b): S1 resumes once S2 is done (gated %s)", str(g).c_str());
    releaseJob(gate, 1);
  }
  std::printf("%s declared, speculative resumes through admission (demand pending, another running)\n",
              failures ? "?   " : "ok  ");
}

// #74 re-review, High: a normal job released from a park, queued behind another released job, is
// promoted ahead of it when demanded. One worker: three jobs park on U; U completes and releases
// them; the first runs (gated); a demand on the last queued one must run it before the middle one.
static std::vector<std::string>
releasedOrder(int demand_rank) {
  Scenario sc;
  sc.add(); // 0: U, submitted after its dependents
  for (int i = 0; i < 3; i++)
    sc.add({0}); // 1-3: normal jobs that park on U
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  std::vector<std::string> order;
  compile::Service<FakeJob *> sched(declared(1, 1));
  for (int i = 1; i <= 3; i++) {
    sched.submit(sc.jobs[i].get());
    gatedNow(gate, 1);
    releaseJob(gate, i); // parks on U
  }
  sched.submit(sc.jobs[0].get());
  gatedNow(gate, 1);
  order.push_back(releaseJob(gate, 0)); // U done: 1-3 released
  auto first = gatedNow(gate, 1);       // the first released job runs (gated)
  std::thread demander;
  std::vector<int> rest;
  for (int i = 1; i <= 3; i++)
    if (!first.count(i))
      rest.push_back(i);
  if (demand_rank >= 0) {
    auto *target = sc.jobs[demand_rank].get();
    gw2::g_promoted_count = 0;
    demander = std::thread([target] { compile::demand(target->record, target->ready); });
    while (gw2::g_promoted_count.load() < 1)
      std::this_thread::yield();
  }
  for (int step = 0; step < 3; step++) {
    auto g = gatedNow(gate, 1);
    if (g.empty())
      break;
    order.push_back(releaseJob(gate, *g.begin()));
  }
  if (demander.joinable())
    demander.join();
  return order;
}

static void
declaredPromoteReleased() {
  auto plain = releasedOrder(-1); // U, then the released jobs in their queue order
  CHECK(plain.size() == 4, "released: %zu outcomes", plain.size());
  if (plain.size() != 4)
    return;
  int last = std::stoi(plain[3]); // the job queued last ("N->done")
  int middle = std::stoi(plain[2]);
  auto demanded = releasedOrder(last);
  std::string want = plain[0] + " " + plain[1] + " " + plain[3] + " " + plain[2];
  std::string got;
  for (auto &o : demanded)
    got += (got.empty() ? "" : " ") + o;
  bool ok = got == want;
  CHECK(ok, "released: demanding job %d queued behind %d runs it first: want %s, got %s", last, middle, want.c_str(),
        got.c_str());
  std::printf("%s declared, a demanded released job is promoted past the other released ones: %s\n", ok ? "ok  " : "FAIL",
              got.c_str());
}

// Waits until `n` jobs are at the gate (each on its own worker); false after 10 s.
static bool
waitGated(Gate &gate, size_t n) {
  std::unique_lock<std::mutex> l(gate.m);
  return gate.cv.wait_for(l, std::chrono::seconds(10), [&] { return gate.gated.size() >= n; });
}

// gw2 (#159): Options::speculative_jobs (DXMT_WARM_JOBS). Slowness comes from the fake clock: a job's
// `cost` is added to it while the job runs, so no real time decides admission.
static void
declaredSpeculativeJobs() {
  constexpr int64_t kSlow = 5 * compile::kSpeculativeSlowNs;
  auto speculative = [](unsigned jobs) {
    auto o = declared(2, 8);
    o.speculative_jobs = jobs;
    return o;
  };
  // one at a time while the runs are fast (N = 3), and with N = 1 while they are slow (today's rule)
  for (auto [jobs, cost, what] : {std::tuple{3u, int64_t(0), "N=3, fast runs"}, std::tuple{1u, kSlow, "N=1, slow runs"}}) {
    Scenario sc;
    for (int i = 0; i < 3; i++)
      sc.add()->cost = cost;
    Gate gate;
    for (auto &j : sc.jobs)
      j->gate = &gate;
    compile::Service<FakeJob *> sched(speculative(jobs));
    CHECK(currentWorkers() == (jobs > 1 ? 4u : 2u), "%s: %zu workers", what, currentWorkers());
    for (auto &j : sc.jobs)
      sched.submit(j.get(), compile::Lane::Speculative);
    for (int i = 0; i < 3; i++) {
      waitGated(gate, 1);
      auto g = gatedNow(gate, currentWorkers());
      CHECK(g == std::set<int>({i}), "%s: one speculative job at a time (gated %s)", what, str(g).c_str());
      releaseJob(gate, i);
    }
  }
  // N = 3, slow runs: three at once; a demand still goes first
  gw2::g_speculative_peak = 0;
  Scenario sc;
  for (int i = 0; i < 6; i++)
    sc.add()->cost = kSlow; // 0-5: speculative
  auto *n6 = sc.add();      // 6: normal, demanded
  Gate gate;
  for (auto &j : sc.jobs)
    j->gate = &gate;
  {
    compile::Service<FakeJob *> sched(speculative(3));
    CHECK(currentWorkers() == 4, "N=3: a worker more than the speculative jobs (%zu)", currentWorkers());
    for (int i = 0; i < 6; i++)
      sched.submit(sc.jobs[i].get(), compile::Lane::Speculative);
    waitGated(gate, 1);
    auto g = gatedNow(gate, 4);
    CHECK(g == std::set<int>({0}), "N=3: one at a time before a run was slow (gated %s)", str(g).c_str());
    releaseJob(gate, 0); // slow: the average is 5 ms
    waitGated(gate, 3);
    g = gatedNow(gate, 4);
    CHECK(g == std::set<int>({1, 2, 3}), "N=3: three at once after a slow run (gated %s)", str(g).c_str());
    CHECK(gw2::g_speculative_peak.load() == 3, "N=3: jobs= peak %u", gw2::g_speculative_peak.load());
    sched.submit(n6);
    waitGated(gate, 4);
    g = gatedNow(gate, 4);
    CHECK(g == std::set<int>({1, 2, 3, 6}), "N=3: the fourth worker takes a normal job (gated %s)", str(g).c_str());
    // a demand pending (on the running normal job): a freed slot stays free
    uint64_t promoted = gw2::g_promoted_count.load();
    std::thread demander([&] { compile::demand(n6->record, n6->ready); });
    while (gw2::g_promoted_count.load() == promoted)
      std::this_thread::yield();
    releaseJob(gate, 1);
    g = gatedNow(gate, 4);
    CHECK(g == std::set<int>({2, 3, 6}), "N=3: no speculative job starts while a demand waits (gated %s)",
          str(g).c_str());
    // a demand on the last queued speculative job: it goes ahead of the one queued before it
    auto *s5 = sc.jobs[5].get();
    std::thread demander5([&] { compile::demand(s5->record, s5->ready); });
    waitGated(gate, 4);
    g = gatedNow(gate, 4);
    CHECK(g == std::set<int>({2, 3, 5, 6}), "N=3: the demanded speculative job runs before job 4 (gated %s)",
          str(g).c_str());
    releaseJob(gate, 5);
    demander5.join();
    g = gatedNow(gate, 4);
    CHECK(g == std::set<int>({2, 3, 6}), "N=3: still none while the first demand waits (gated %s)", str(g).c_str());
    releaseJob(gate, 6);
    demander.join();
    waitGated(gate, 3);
    g = gatedNow(gate, 4);
    CHECK(g == std::set<int>({2, 3, 4}), "N=3: admission resumes up to three after the demands (gated %s)",
          str(g).c_str());
    for (int i : {2, 3, 4})
      releaseJob(gate, i);
  }
  bool once = std::all_of(sc.jobs.begin(), sc.jobs.end(), [](auto &j) { return j->done_count.load() == 1; });
  CHECK(once, "N=3: every job completed once");
  std::printf("%s declared, DXMT_WARM_JOBS: one at a time while fast or N=1, up to N while slow, demands first\n",
              failures ? "?   " : "ok  ");
}

// gw2 (#152): with each DXMT_COMPILE_PRIORITY setting and either policy, every worker, the first two
// and those a submit adds, applies Options::priority once, on itself, before it runs a job; the jobs
// all complete.
static void
workerPriority() {
  using gw2::ThreadPriority;
  const std::pair<ThreadPriority, const char *> modes[] = {
      {ThreadPriority::Critical, "critical"}, {ThreadPriority::Normal, "normal"}, {ThreadPriority::Utility, "utility"}};
  for (auto [mode, mode_name] : modes)
    for (auto policy : {compile::Policy::Legacy, compile::Policy::Declared}) {
      const char *policy_name = policy == compile::Policy::Legacy ? "legacy" : "declared";
      {
        std::lock_guard<std::mutex> l(g_priority_mutex);
        g_priority_calls.clear();
      }
      Scenario sc;
      Gate gate;
      for (int i = 0; i < 4; i++)
        sc.add()->gate = &gate;
      {
        compile::Options o;
        o.initial_workers = 2;
        o.max_workers = 4;
        o.priority = mode;
        o.policy = policy;
        compile::Service<FakeJob *> sched(o);
        // two jobs for the first two workers, then one at a time while every worker is busy: each adds one
        sched.submit(sc.jobs[0].get());
        sched.submit(sc.jobs[1].get());
        bool gated = true;
        for (size_t i = 2; i < 4; i++) {
          gated = waitGated(gate, i) && gated;
          sched.submit(sc.jobs[i].get());
        }
        gated = waitGated(gate, 4) && gated;
        CHECK(gated, "%s, %s: the workers didn't all reach the gate in 10 s", mode_name, policy_name);
        for (auto &j : sc.jobs)
          releaseJob(gate, j->id);
        for (auto &j : sc.jobs)
          j->ready.wait(false);
      } // workers joined
      bool all_done = std::all_of(sc.jobs.begin(), sc.jobs.end(), [](auto &j) { return j->done_count.load() == 1; });
      std::lock_guard<std::mutex> l(g_priority_mutex);
      std::set<std::thread::id> threads;
      bool right = true;
      for (auto &c : g_priority_calls) {
        threads.insert(c.thread);
        right = right && c.mode == mode && !c.after_a_job;
      }
      bool ok = all_done && g_priority_calls.size() == 4 && threads.size() == 4 && right;
      CHECK(ok, "%s, %s: %zu calls on %zu threads, mode and order %s, jobs %s", mode_name, policy_name,
            g_priority_calls.size(), threads.size(), right ? "right" : "wrong", all_done ? "done once" : "not done once");
      std::printf("%s worker priority %s, %s: 4 workers, each applied it once before its first job\n",
                  ok ? "ok  " : "FAIL", mode_name, policy_name);
    }
}

int
main() {
  parity("chain", chain);
  parity("shared variants", sharedVariants);
  parity("continuations first", continuationsFirst);
  parity("parked before FIFO", parkedBeforeFifo);
  parity("failures", failures_);
  parity("spurious dependency", spurious);
  parity("late submits", lateSubmits);
  parity("GW2 burst", burst);
  parity("wide", wide);
  parity("growth to the limit", growth);
  g_timing = true;
  parity("GW2 burst, STATS on", burst);
  g_timing = false;
  recordsAndDemands();
  declaredNoPark();
  declaredDemand();
  declaredSpeculative();
  declaredUnknownDependency();
  declaredLateSpeculativeDependency();
  declaredSpeculativeResume();
  declaredPromoteReleased();
  declaredStress();
  declaredSpeculativeJobs();
  workerPriority();
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
