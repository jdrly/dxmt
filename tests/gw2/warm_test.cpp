// gw2-test: pure
// gw2-build: clang -c -O1 src/util/sha1/sha1.c -o ${OUT}_sha1.o
// gw2-build: clang++ -std=c++20 -O1 -Itests/gw2/shim -Itests/gw2 -Isrc/dxmt -Isrc/util -Isrc/winemetal tests/gw2/warm_test.cpp src/dxmt/dxmt_gw2_warm.cpp src/dxmt/dxmt_compile_recipe.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp src/util/sha1/sha1_util.cpp ${OUT}_sha1.o -o $OUT
//
// gw2 (C4, W2/W3): native test of the shared results (src/dxmt/dxmt_compile_results.hpp) with the
// compile service's declared policy, and of the warm plan (src/dxmt/dxmt_gw2_warm.cpp). No Metal.
//
// - Exactly once: live and speculative (warm) jobs racing for overlapping keys on the real service,
//   from several submitting threads, some warm builds failing (abandoned): each key is built
//   successfully once, every job completes, a live job that finds a warm job building its key parks
//   on it and then takes its result.
// - Joins and demand: a live job parked on a running warm job wakes when it publishes; a demand on a
//   live job parked on a queued warm job promotes the warm job past the speculative limits.
// - The plan is bounded: DXMT_WARM_MAX recipes at most, the rest counted as capped; variants once
//   each; other Metal targets and malformed records skipped; recipes deserialize back to their key.

#include "dxmt_compile_results.hpp"
#include "dxmt_compile_service.hpp"
#include "dxmt_gw2_warm.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace dxmt;

// the telemetry the service calls (production: dxmt_gw2_compile.cpp)
namespace dxmt::gw2 {
void compileQueue(int64_t) {}
void compileThreads(uint64_t) {}
bool compileTiming() { return false; }
int64_t compileNowNs() { return 0; }
std::atomic<uint64_t> g_waits{0};
CompileWaitScope::CompileWaitScope(const compile::JobRecord *) { g_waits++; }
CompileWaitScope::~CompileWaitScope() {}
void compilePromoted() {}
} // namespace dxmt::gw2
extern "C" void NSObject_retain(obj_handle_t) {}
extern "C" void NSObject_release(obj_handle_t) {}

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

// --- jobs over a shared table, as the variant/pipeline jobs and the warm jobs use it

struct Job;
using Table = compile::ResultTable<int, int, Job *>;
using Claim = Table::Claim;

struct Builds {
  std::mutex m;
  std::map<int, int> ok, failed; // per key
};

struct Job {
  int key;
  bool warm;
  bool fails = false; // a warm build that fails (no AIR): abandoned
  Table *table;
  Builds *builds;
  std::vector<Job *> deps;
  std::atomic_bool ready{false};
  std::atomic<int> done_count{0};
  int result = -1;
  bool joined = false;
  compile::JobRecord record;
  // the gate: a job with a gate waits there before it builds (to hold a claim while others arrive)
  std::atomic<bool> *gate = nullptr;

  Job *
  run() {
    int value = 0;
    Job *owner = nullptr;
    switch (table->claim(key, this, &value, &owner)) {
    case Claim::Done:
      result = value;
      return this;
    case Claim::Busy:
      if (warm)
        return this; // a warm job never waits: someone else builds it
      joined = true;
      return owner;
    case Claim::Mine:
      break;
    }
    if (gate)
      while (!gate->load())
        std::this_thread::yield();
    for (volatile int spin = 0; spin < 500; spin = spin + 1)
      ;
    if (fails) {
      std::lock_guard<std::mutex> l(builds->m);
      builds->failed[key]++;
      table->abandon(key, this);
      return this;
    }
    {
      std::lock_guard<std::mutex> l(builds->m);
      builds->ok[key]++;
    }
    result = key * 10;
    table->publish(key, this, result);
    return this;
  }
  bool GetIsDone() { return ready.load(); }
  void SetIsDone(bool v) {
    done_count++;
    ready.store(v);
    ready.notify_all();
  }
};

template <> struct dxmt::compile::JobTraits<Job *> {
  Job *run(Job *job) { return job->run(); }
  bool done(Job *job) { return job->GetIsDone(); }
  void setDone(Job *job) { job->SetIsDone(true); }
  compile::JobRecord &record(Job *job) { return job->record; }
  void deps(Job *job, std::vector<Job *> &out) { out.insert(out.end(), job->deps.begin(), job->deps.end()); }
};

static compile::Options
declared(unsigned workers, unsigned max) {
  compile::Options o;
  o.initial_workers = workers;
  o.max_workers = max;
  o.policy = compile::Policy::Declared;
  return o;
}

static void
exactlyOnce(int round) {
  Table table;
  Builds builds;
  std::vector<std::unique_ptr<Job>> jobs;
  std::mt19937 rng(round);
  constexpr int kKeys = 600, kWarm = 600, kLive = 1800;
  // the warm thread's jobs: every key once, in order; a few fail (no AIR)
  for (int k = 0; k < kWarm; k++) {
    auto j = std::make_unique<Job>();
    j->key = k % kKeys;
    j->warm = true;
    j->fails = rng() % 10 == 0;
    jobs.push_back(std::move(j));
  }
  // live jobs: each key about three times, in random order (variants shared by many pipelines)
  for (int i = 0; i < kLive; i++) {
    auto j = std::make_unique<Job>();
    j->key = int(rng() % kKeys);
    j->warm = false;
    jobs.push_back(std::move(j));
  }
  for (auto &j : jobs) {
    j->table = &table;
    j->builds = &builds;
  }
  {
    compile::Service<Job *> service(declared(2, 0));
    std::thread warm_thread([&] {
      for (int i = 0; i < kWarm; i++)
        service.submit(jobs[i].get(), compile::Lane::Speculative);
    });
    std::vector<std::thread> live;
    for (int t = 0; t < 3; t++)
      live.emplace_back([&, t] {
        for (int i = kWarm + t; i < kWarm + kLive; i += 3)
          service.submit(jobs[i].get());
      });
    warm_thread.join();
    for (auto &t : live)
      t.join();
    // demands on some live jobs while it runs, as the encode thread would
    for (int i = kWarm; i < kWarm + kLive; i += 97)
      compile::demand(jobs[i]->record, jobs[i]->ready);
    for (auto &j : jobs)
      j->ready.wait(false);
  }
  int twice = 0, done_once = 0, wrong = 0, joins = 0, unbuilt = 0;
  for (auto &[k, n] : builds.ok)
    twice += n > 1;
  for (auto &j : jobs) {
    done_once += j->done_count.load() == 1;
    joins += j->joined;
    if (!j->warm && j->result != j->key * 10)
      wrong++;
  }
  for (int k = 0; k < kKeys; k++)
    unbuilt += builds.ok.count(k) == 0 && std::any_of(jobs.begin() + kWarm, jobs.end(), [&](auto &j) { return j->key == k; });
  CHECK(twice == 0, "round %d: %d keys built twice", round, twice);
  CHECK(done_once == int(jobs.size()), "round %d: %d of %zu jobs completed once", round, done_once, jobs.size());
  CHECK(wrong == 0, "round %d: %d live jobs without their key's result", round, wrong);
  CHECK(unbuilt == 0, "round %d: %d keys a live job needed were never built", round, unbuilt);
  size_t failed = 0;
  for (auto &[k, n] : builds.failed)
    failed += n;
  if (round == 0)
    std::printf("     round 0: %zu keys built once each (of %d), %zu warm builds abandoned, %d live joins on a "
                "building owner\n",
                builds.ok.size(), kKeys, failed, joins);
}

// A live job finds a warm job building its key: it parks on it, a demand on it promotes nothing that
// would wait (the owner runs), and it takes the warm result when the warm job publishes.
static void
joinAndDemand() {
  Table table;
  Builds builds;
  std::atomic<bool> open{false};
  Job warm, live;
  warm.key = live.key = 7;
  warm.warm = true;
  live.warm = false;
  warm.gate = &open;
  for (auto *j : {&warm, &live}) {
    j->table = &table;
    j->builds = &builds;
  }
  {
    compile::Service<Job *> service(declared(2, 2));
    service.submit(&warm, compile::Lane::Speculative);
    while (table.size() == 0) // the warm job holds the claim
      std::this_thread::yield();
    service.submit(&live);
    while (!live.joined)
      std::this_thread::yield();
    std::thread demander([&] { compile::demand(live.record, live.ready); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!live.ready.load(), "the live job waits for the warm owner");
    open = true;
    demander.join();
  }
  CHECK(live.result == 70 && builds.ok[7] == 1, "the live job took the warm result (built %d)", builds.ok[7]);
  std::printf("%s join: a live job parks on the warm owner, takes its result; built once\n",
              live.result == 70 && builds.ok[7] == 1 ? "ok  " : "FAIL");
}

// A live job demanded while 50 warm jobs are queued (one running) doesn't wait for any of them.
static void
demandNotDelayed() {
  Table table;
  Builds builds;
  std::atomic<bool> open{false};
  std::vector<std::unique_ptr<Job>> warm;
  for (int i = 0; i < 50; i++) {
    auto j = std::make_unique<Job>();
    j->key = 100 + i;
    j->warm = true;
    j->table = &table;
    j->builds = &builds;
    warm.push_back(std::move(j));
  }
  warm[0]->gate = &open; // the one running speculative job, held
  Job live;
  live.key = 1;
  live.warm = false;
  live.table = &table;
  live.builds = &builds;
  {
    compile::Service<Job *> service(declared(2, 2));
    for (auto &j : warm)
      service.submit(j.get(), compile::Lane::Speculative);
    while (table.size() == 0)
      std::this_thread::yield();
    auto t0 = std::chrono::steady_clock::now();
    service.submit(&live);
    compile::demand(live.record, live.ready);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    size_t warm_done = 0;
    for (auto &j : warm)
      warm_done += j->GetIsDone();
    CHECK(live.result == 10 && warm_done == 0, "the demanded live job ran first (%zu warm jobs done)", warm_done);
    std::printf("%s demand: a live job submitted behind 50 warm jobs (one running) completes in %.2f ms, none "
                "of the queued warm jobs before it\n",
                live.result == 10 && warm_done == 0 ? "ok  " : "FAIL", ms);
    open = true;
    for (auto &j : warm)
      j->ready.wait(false);
  }
}

// --- the plan

static std::vector<uint8_t>
recordedRecipe(uint32_t target, int vs, int ps) {
  compile::PipelineRecipe r;
  WMTRenderPipelineInfo info{};
  WMT::InitializeRenderPipelineInfo(info);
  info.colors[0].pixel_format = WMTPixelFormat(70 + vs % 3);
  r = compile::recipeFromInfo(info);
  r.metal_version = target;
  uint32_t a = vs, b = vs + 1000, c = ps, d = ps + 2000;
  r.vs = compile::VariantKey{Sha1HashState::compute(&a, 4), Sha1HashState::compute(&b, 4)};
  if (ps >= 0)
    r.ps = compile::VariantKey{Sha1HashState::compute(&c, 4), Sha1HashState::compute(&d, 4)};
  gw2::RecipeBytes bytes;
  compile::serialize(r, bytes);
  // deserialize gives the same recipe back (its key)
  compile::PipelineRecipe back;
  if (!compile::deserialize(bytes.data.data(), bytes.data.size(), back) ||
      compile::recipeKey(back) != compile::recipeKey(r))
    failures++, std::printf("FAIL: a recipe didn't deserialize to itself\n");
  return bytes.data;
}

static void
plan() {
  gw2::WarmPlan p;
  int n = 0;
  // 5,000 recipes over 300 vertex and 500 pixel variants; every tenth for another target
  for (int i = 0; i < 5000; i++, n++) {
    auto rec = recordedRecipe(i % 10 == 0 ? 310 : 320, i % 300, i % 7 == 0 ? -1 : i % 500);
    gw2::warmPlanAdd(p, rec.data(), rec.size(), 320, gw2::WarmMode::All, 2048);
  }
  uint8_t junk[7] = {1, 0, 0, 0, 9, 9, 9};
  gw2::warmPlanAdd(p, junk, sizeof(junk), 320, gw2::WarmMode::All, 2048);
  auto short_rec = recordedRecipe(320, 1, 1);
  gw2::warmPlanAdd(p, short_rec.data(), short_rec.size() - 1, 320, gw2::WarmMode::All, 2048);
  bool bounded = p.recipes == 2048 && p.pipelines.size() == 2048 && p.variants.size() <= 2 * 2048;
  CHECK(bounded, "plan bounded: %llu recipes, %zu pipelines, %zu variants", (unsigned long long)p.recipes,
        p.pipelines.size(), p.variants.size());
  CHECK(p.other_target > 0 && p.malformed == 2, "skipped: other target %llu, malformed %llu",
        (unsigned long long)p.other_target, (unsigned long long)p.malformed);
  CHECK(p.recipes + p.capped + p.other_target + p.malformed == uint64_t(n) + 2, "every record accounted for");
  std::set<std::string> names;
  for (auto &v : p.variants)
    names.insert(gw2::warmFunctionName(v));
  CHECK(names.size() == p.variants.size(), "variants once each");
  bool vs_name = gw2::warmFunctionName(p.variants[0]).rfind("vs_", 0) == 0 &&
                 gw2::warmFunctionName(p.variants[0]).size() == 3 + 8 + 1 + 40;
  CHECK(vs_name, "function names as CreateVariantShader's: %s", gw2::warmFunctionName(p.variants[0]).c_str());
  gw2::WarmPlan f;
  for (int i = 0; i < 100; i++) {
    auto rec = recordedRecipe(320, i % 30, i % 40);
    gw2::warmPlanAdd(f, rec.data(), rec.size(), 320, gw2::WarmMode::Functions, 2048);
  }
  CHECK(f.pipelines.empty() && f.variants.size() == 30 + 40, "functions only: %zu variants, %zu pipelines",
        f.variants.size(), f.pipelines.size());
  std::printf("%s plan: %llu of %d recipes planned (cap 2048), %llu capped, %llu other target, %llu malformed; %zu "
              "variants once each\n",
              bounded ? "ok  " : "FAIL", (unsigned long long)p.recipes, n, (unsigned long long)p.capped,
              (unsigned long long)p.other_target, (unsigned long long)p.malformed, p.variants.size());
}

int
main() {
  for (int round = 0; round < 10; round++)
    exactlyOnce(round);
  std::printf("%s exactly once: 10 rounds of 600 warm + 1,800 live jobs over 600 keys, 4 submitting threads\n",
              failures ? "FAIL" : "ok  ");
  joinAndDemand();
  demandNotDelayed();
  plan();
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
