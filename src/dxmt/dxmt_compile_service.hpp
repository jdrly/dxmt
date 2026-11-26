#pragma once

// gw2 (C1, architecture council §1): the compile service. One owner for compile jobs (shader
// variants and pipelines), their state and the demands that wait on them.
//
// C1 is the `legacy` policy only, with no behaviour change: the scheduling loop is task_scheduler's
// (src/dxmt/dxmt_tasks.hpp up to 0034ffa), moved here as is (the council's run/park adapter):
//
// - submitted jobs queue at once, FIFO;
// - a worker runs a job; the job probes its dependencies itself and returns the first that isn't
//   done (it is parked on it, and requeued when that completes) or itself when it is done;
// - a returned dependency that is already done reruns the job at once (the spurious-dependency
//   check, under the dependency lock);
// - parked jobs released by a completion go to a continuation queue that workers take first;
// - 2 workers at the start; a submit that finds every worker running adds one, up to 2 x CPUs;
//   all TIME_CRITICAL.
//
// What C1 adds: each job's state (queued, running, parked, building, complete, and what it is parked
// on) is owned here and kept with DXMT_STATS off too; STATS only adds the timeline (timestamps) that
// the COMPILE section's wait causes read. A demand (a thread blocking on a job: `demand`) is one
// function instead of four copies; the COMPILE fields are unchanged.
//
// C2: the `declared` policy (`DXMT_COMPILE_DEMAND=1`, off by default; legacy stays exactly as above):
//
// - a job's dependencies are taken when it is submitted (JobTraits::deps, the shader variants a
//   pipeline's constructor already holds); a job with unfinished dependencies waits, unqueued, and
//   becomes runnable when the last completes, so no worker runs it just to park it. A dependency the
//   service doesn't know (never submitted) isn't waited for: the job runs and parks on it as before;
// - lanes: a demand queue first, then released parked jobs (as legacy's continuations), then normal
//   FIFO, then speculative jobs (warming, W2/W3): at most one speculative job runs at a time, and none
//   starts while a demand is pending;
// - a demand promotes the job and its unfinished dependency closure, once each: a queued job moves to
//   the demand queue, a waiting one goes there when it becomes runnable. Running jobs finish as they
//   are; nothing is built twice.
//
// Not yet: owning jobs (the variant and pipeline maps keep owning them) and stable keys and shared
// results (C3/C4), which warming needs to submit jobs without live D3D11 objects.

#include "dxmt_gw2_compile.hpp"
#include "thread.hpp"
#include "util_win32_compat.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt::compile {

enum class Policy : uint8_t { Legacy, Declared };
enum class Lane : uint8_t { Normal, Demand, Speculative };

// Where a demand promotes its job (the declared policy's service); none under legacy.
class DemandSink {
public:
  virtual void beginDemand(const struct JobRecord &r) = 0;
  virtual void endDemand() = 0;

protected:
  ~DemandSink() = default;
};

enum class JobState : uint8_t { Absent = 0, Queued, Running, Parked, Building, Complete };

// A job's record, owned by the service (the job object embeds it). State and `parked_on` are always
// kept; the timeline only when STATS was on at submit (`timed`), as before (#46): a job submitted with
// STATS off isn't classified when it is demanded.
struct JobRecord {
  std::atomic<JobState> state{JobState::Absent};
  std::atomic<const JobRecord *> parked_on{nullptr}; // the dependency it was last parked on
  std::atomic<bool> timed{false};
  std::atomic<DemandSink *> sink{nullptr}; // C2: the declared service that queued it
  std::atomic<int64_t> submit_ns{0}, start_ns{0}, build_ns{0}, done_ns{0}, parked_at_ns{0}, parked_ns{0};
};

namespace detail {

inline void
onSubmit(JobRecord &r) {
  bool timed = gw2::compileTiming();
  r.timed.store(timed, std::memory_order_relaxed);
  r.parked_on.store(nullptr, std::memory_order_relaxed);
  if (timed) {
    r.start_ns.store(0, std::memory_order_relaxed);
    r.build_ns.store(0, std::memory_order_relaxed);
    r.done_ns.store(0, std::memory_order_relaxed);
    r.parked_ns.store(0, std::memory_order_relaxed);
    r.parked_at_ns.store(0, std::memory_order_relaxed);
    r.submit_ns.store(gw2::compileNowNs(), std::memory_order_relaxed);
  }
  r.state.store(JobState::Queued, std::memory_order_release);
}

// a worker starts or resumes it
inline void
onRun(JobRecord &r) {
  if (r.timed.load(std::memory_order_relaxed)) {
    auto now = gw2::compileNowNs();
    if (!r.start_ns.load(std::memory_order_relaxed))
      r.start_ns.store(now, std::memory_order_relaxed);
    if (r.state.load(std::memory_order_acquire) == JobState::Parked)
      r.parked_ns.fetch_add(now - r.parked_at_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
  r.state.store(JobState::Running, std::memory_order_release);
}

// it returned a dependency
inline void
onParked(JobRecord &r, const JobRecord *dep) {
  if (r.timed.load(std::memory_order_relaxed))
    r.parked_at_ns.store(gw2::compileNowNs(), std::memory_order_relaxed);
  r.parked_on.store(dep, std::memory_order_relaxed);
  r.state.store(JobState::Parked, std::memory_order_release);
}

inline void
onComplete(JobRecord &r) {
  if (r.timed.load(std::memory_order_relaxed))
    r.done_ns.store(gw2::compileNowNs(), std::memory_order_relaxed);
  r.state.store(JobState::Complete, std::memory_order_release);
}

} // namespace detail

// The job's Metal build starts (pipeline jobs, right before newRenderPipelineState and the like).
inline void
building(JobRecord &r) {
  if (r.timed.load(std::memory_order_relaxed))
    r.build_ns.store(gw2::compileNowNs(), std::memory_order_relaxed);
  r.state.store(JobState::Building, std::memory_order_release);
}

// A demand: the calling thread blocks until the job is done (`ready`, the job's own flag, set and
// notified by its SetIsDone). The wait is counted and classified by the job's state when it began
// (#46's waitcause); nothing is recorded when the job is already done.
// C2: under the declared policy the demand also promotes the job (after it is classified).
inline void
demand(const JobRecord &r, std::atomic_bool &ready) {
  if (!ready.load(std::memory_order_acquire)) {
    gw2::CompileWaitScope wait(&r);
    auto *sink = r.sink.load(std::memory_order_acquire);
    if (sink)
      sink->beginDemand(r);
    ready.wait(false, std::memory_order_acquire);
    if (sink)
      sink->endDemand();
  }
}

// How the service drives a job type: run it (it returns itself when done, else the dependency it
// waits on), read and set its done flag, and reach its record.
template <typename Job> struct JobTraits {
  Job run(Job job);
  bool done(Job job);
  void setDone(Job job);
  JobRecord &record(Job job);
  void deps(Job job, std::vector<Job> &out); // C2: its dependencies, in probe order
};

struct Options {
  unsigned initial_workers = 2;
  unsigned max_workers = 0; // 0: 2 x CPUs
  int priority = THREAD_PRIORITY_TIME_CRITICAL;
  Policy policy = Policy::Legacy;
};

template <typename Job, typename Traits = JobTraits<Job>> class Service final : public DemandSink {
public:
  explicit Service(Options options = {});
  ~Service(); // stop: workers finish the job they run, nothing else; queued jobs are dropped (as before)

  // Queues a new job, never blocking on a build. Legacy: FIFO, `lane` ignored. Declared: after its
  // unfinished dependencies, in `lane`. C4 adds acquire(key, lane, deps, job).
  void submit(Job job, Lane lane = Lane::Normal);

  // DemandSink (declared policy): compile::demand calls these around its wait.
  void beginDemand(const JobRecord &r) override;
  void endDemand() override;

  uint64_t
  runningWorkers() {
    return running_.load(std::memory_order_relaxed);
  }

private:
  void workerLoop();
  void workerLoopDeclared();

  // --- declared policy (all under worker_mutex_)
  struct Node {
    uint32_t pending = 0;        // unfinished dependencies
    std::vector<Job> dependents; // jobs waiting for this one
    Lane lane = Lane::Normal;
    bool promoted = false;
    bool runnable = false; // queued or running
  };
  void makeRunnable(Job job, Node &node);
  void promote(Job job);
  void promoteRecord(const JobRecord *record);
  void release(Job job);
  bool takeable() const;

  Traits traits_;
  std::unordered_map<Job, Node> nodes_;                  // submitted, not complete
  std::unordered_map<const JobRecord *, Job> by_record_; // the same jobs, by record (demand)
  // demanded dependencies not submitted yet (by record): promoted when they are
  std::unordered_set<const JobRecord *> promote_on_submit_;
  // released_q_: normal jobs a completion released from a park (legacy's continuations, searchable)
  std::deque<Job> demand_q_, released_q_, normal_q_, speculative_q_;
  uint64_t demands_ = 0;           // demands waiting now
  uint32_t speculative_running_ = 0;

  Options options_;
  dxmt::mutex worker_mutex_;
  dxmt::condition_variable worker_cond_;
  std::queue<Job> queue_;
  std::queue<Job> continuations_;

  dxmt::mutex deps_mutex_;
  std::unordered_multimap<Job, Job> parked_; // dependency -> the jobs parked on it

  std::vector<dxmt::thread> workers_;

  std::atomic_bool destroyed_ = false;
  std::atomic_uint64_t running_ = 0;
  uint64_t threads_;
  uint64_t max_threads_;
};

template <typename Job, typename Traits> Service<Job, Traits>::Service(Options options) : options_(options) {
  max_threads_ = options_.max_workers ? options_.max_workers : dxmt::thread::hardware_concurrency() * 2;
  workers_.reserve(max_threads_);
  threads_ = options_.initial_workers;

  for (unsigned i = 0; i < threads_; i++) {
    workers_.emplace_back([this]() { workerLoop(); });
  }
  gw2::compileThreads(threads_); // gw2 (#5)
}

template <typename Job, typename Traits> Service<Job, Traits>::~Service() {
  {
    // gw2 (#72): under the lock. A worker checks its wait's predicate under worker_mutex_ and then
    // blocks; an unlocked store and notify could fall between the two, the worker would never wake,
    // and the join below would hang the device's teardown (upstream's task_scheduler had the same).
    std::unique_lock<dxmt::mutex> lock(worker_mutex_);
    destroyed_.store(true);
  }
  worker_cond_.notify_all();

  for (auto &worker : workers_) {
    if (worker.joinable())
      worker.join();
  }

  workers_.clear();
}

template <typename Job, typename Traits>
void
Service<Job, Traits>::workerLoop() {
  if (options_.policy == Policy::Declared) {
    workerLoopDeclared();
    return;
  }
  Traits traits;
  std::vector<Job> released;
  SetThreadPriority(GetCurrentThread(), options_.priority);
  while (!destroyed_.load()) {
    Job job;
    {
      std::unique_lock<dxmt::mutex> lock(worker_mutex_);

      if (queue_.empty() && continuations_.empty()) {
        worker_cond_.wait(lock, [this]() { return queue_.size() || continuations_.size() || destroyed_.load(); });
      }

      if (!continuations_.empty()) {
        job = continuations_.front();
        continuations_.pop();
      } else if (!queue_.empty()) {
        job = queue_.front();
        queue_.pop();
      } else {
        break;
      }
    }
    running_.fetch_add(1, std::memory_order_relaxed);
    while (true) {
      detail::onRun(traits.record(job));
      Job next = traits.run(job);
      if (next == job) {
        {
          std::unique_lock<dxmt::mutex> lock(deps_mutex_);
          auto range = parked_.equal_range(next);
          for (auto itr = range.first; itr != range.second; ++itr) {
            released.push_back(itr->second);
          }
          parked_.erase(range.first, range.second);
          detail::onComplete(traits.record(next)); // before the demand wakes
          traits.setDone(next);
        }
        gw2::compileQueue(-1); // gw2 (#5): a submitted job completed
        {
          std::unique_lock<dxmt::mutex> lock(worker_mutex_);
          for (auto &j : released) {
            continuations_.push(j);
          }
        }
        worker_cond_.notify_all();
        released.clear();
      } else {
        detail::onParked(traits.record(job), &traits.record(next));
        std::unique_lock<dxmt::mutex> lock(deps_mutex_);
        // spurious dependency: it completed since the job probed it
        if (traits.done(next)) {
          continue;
        }
        parked_.insert({next, job});
      }
      break;
    }
    running_.fetch_sub(1, std::memory_order_relaxed);
  }
}

template <typename Job, typename Traits>
void
Service<Job, Traits>::submit(Job job, Lane lane) {
  detail::onSubmit(Traits{}.record(job)); // before any worker can take it
  if (options_.policy == Policy::Declared) {
    auto &record = traits_.record(job);
    record.sink.store(this, std::memory_order_release);
    std::vector<Job> deps;
    traits_.deps(job, deps);
    std::unique_lock<dxmt::mutex> lock(worker_mutex_);
    auto &node = nodes_[job];
    node.lane = lane;
    by_record_[&record] = job;
    for (auto dep : deps) {
      // a dependency completes (done, then its node goes) under this lock's order: not done and
      // still known means it will release this job; not known and not done means never submitted
      if (traits_.done(dep))
        continue;
      if (auto it = nodes_.find(dep); it != nodes_.end()) {
        it->second.dependents.push_back(job);
        node.pending++;
      }
    }
    gw2::compileQueue(1); // gw2 (#5)
    if (node.pending == 0)
      makeRunnable(job, node);
    if (promote_on_submit_.erase(&record))
      promote(job); // a demand is waiting for it (through a job that parked on it)
    if (running_.load(std::memory_order_relaxed) == threads_ && threads_ < max_threads_) {
      workers_.emplace_back([this]() { workerLoop(); });
      threads_++;
      gw2::compileThreads(threads_); // gw2 (#5)
    }
    worker_cond_.notify_one();
    return;
  }
  std::unique_lock<dxmt::mutex> lock(worker_mutex_);
  queue_.push(job);
  gw2::compileQueue(1); // gw2 (#5): compile queue depth = submitted, not yet completed

  if (running_.load(std::memory_order_relaxed) == threads_ && threads_ < max_threads_) {
    workers_.emplace_back([this]() { workerLoop(); });
    threads_++;
    gw2::compileThreads(threads_); // gw2 (#5)
  }

  worker_cond_.notify_one();
}

// --- C2: the declared policy

template <typename Job, typename Traits>
void
Service<Job, Traits>::makeRunnable(Job job, Node &node) {
  node.runnable = true;
  if (node.promoted || node.lane == Lane::Demand)
    demand_q_.push_back(job);
  else if (node.lane == Lane::Speculative)
    speculative_q_.push_back(job);
  else
    normal_q_.push_back(job);
}

// Promotes `job` and its unfinished dependencies, once each (worker_mutex_ held).
template <typename Job, typename Traits>
void
Service<Job, Traits>::promote(Job job) {
  auto it = nodes_.find(job);
  if (it == nodes_.end() || it->second.promoted)
    return;
  auto &node = it->second;
  node.promoted = true;
  gw2::compilePromoted(); // gw2 (C2)
  if (node.runnable) {
    // queued: move it to the demand queue (running: it finishes as it is)
    for (auto *q : {&released_q_, &normal_q_, &speculative_q_}) {
      if (auto pos = std::find(q->begin(), q->end(), job); pos != q->end()) {
        q->erase(pos);
        demand_q_.push_back(job);
        break;
      }
    }
  }
  std::vector<Job> deps;
  traits_.deps(job, deps);
  for (auto dep : deps)
    promoteRecord(&traits_.record(dep));
  // and what it is parked on, declared or not
  if (auto *dep = traits_.record(job).parked_on.load(std::memory_order_relaxed))
    promoteRecord(dep);
}

// Promotes the job behind `record` if the service has it; if it hasn't been submitted (and isn't
// complete), it is promoted when it is (worker_mutex_ held).
template <typename Job, typename Traits>
void
Service<Job, Traits>::promoteRecord(const JobRecord *record) {
  if (record->state.load(std::memory_order_acquire) == JobState::Complete)
    return;
  if (auto it = by_record_.find(record); it != by_record_.end())
    promote(it->second);
  else
    promote_on_submit_.insert(record);
}

// A parked job its dependency released: back by its lane (worker_mutex_ held). A promoted job goes
// to the demand queue, a speculative one through speculative admission again, a normal one to the
// released queue (taken before the normal FIFO, as legacy's continuations are).
template <typename Job, typename Traits>
void
Service<Job, Traits>::release(Job job) {
  auto it = nodes_.find(job);
  if (it != nodes_.end() && it->second.promoted)
    demand_q_.push_back(job);
  else if (it != nodes_.end() && it->second.lane == Lane::Speculative)
    speculative_q_.push_back(job);
  else
    released_q_.push_back(job);
}

template <typename Job, typename Traits>
void
Service<Job, Traits>::beginDemand(const JobRecord &r) {
  {
    std::unique_lock<dxmt::mutex> lock(worker_mutex_);
    demands_++;
    if (auto it = by_record_.find(&r); it != by_record_.end())
      promote(it->second);
  }
  worker_cond_.notify_all();
}

template <typename Job, typename Traits>
void
Service<Job, Traits>::endDemand() {
  {
    std::unique_lock<dxmt::mutex> lock(worker_mutex_);
    if (--demands_ == 0)
      promote_on_submit_.clear(); // no demand left to inherit them (and records may be reused)
  }
  worker_cond_.notify_all(); // speculative admission may resume
}

template <typename Job, typename Traits>
bool
Service<Job, Traits>::takeable() const {
  return !demand_q_.empty() || !released_q_.empty() || !normal_q_.empty() ||
         (!speculative_q_.empty() && demands_ == 0 && speculative_running_ == 0);
}

template <typename Job, typename Traits>
void
Service<Job, Traits>::workerLoopDeclared() {
  std::vector<Job> released;
  SetThreadPriority(GetCurrentThread(), options_.priority);
  while (!destroyed_.load()) {
    Job job;
    bool speculative = false;
    {
      std::unique_lock<dxmt::mutex> lock(worker_mutex_);
      worker_cond_.wait(lock, [this]() { return takeable() || destroyed_.load(); });
      if (destroyed_.load())
        break;
      if (!demand_q_.empty()) {
        job = demand_q_.front();
        demand_q_.pop_front();
      } else if (!released_q_.empty()) {
        job = released_q_.front();
        released_q_.pop_front();
      } else if (!normal_q_.empty()) {
        job = normal_q_.front();
        normal_q_.pop_front();
      } else {
        job = speculative_q_.front();
        speculative_q_.pop_front();
        speculative = true;
        speculative_running_++;
      }
    }
    running_.fetch_add(1, std::memory_order_relaxed);
    while (true) {
      detail::onRun(traits_.record(job));
      Job next = traits_.run(job);
      if (next == job) {
        {
          std::unique_lock<dxmt::mutex> lock(deps_mutex_);
          auto range = parked_.equal_range(next);
          for (auto itr = range.first; itr != range.second; ++itr)
            released.push_back(itr->second);
          parked_.erase(range.first, range.second);
          detail::onComplete(traits_.record(next)); // before the demand wakes
          traits_.setDone(next);
        }
        gw2::compileQueue(-1); // gw2 (#5)
        {
          std::unique_lock<dxmt::mutex> lock(worker_mutex_);
          for (auto &j : released)
            release(j); // parked on a dependency the service didn't know
          if (auto it = nodes_.find(job); it != nodes_.end()) {
            for (auto dependent : it->second.dependents) {
              auto d = nodes_.find(dependent);
              if (d != nodes_.end() && --d->second.pending == 0)
                makeRunnable(dependent, d->second);
            }
            nodes_.erase(it);
          }
          by_record_.erase(&traits_.record(job));
          if (speculative)
            speculative_running_--;
        }
        worker_cond_.notify_all();
        released.clear();
      } else {
        // a dependency it didn't declare, or one the service didn't know: park as legacy does
        detail::onParked(traits_.record(job), &traits_.record(next));
        std::unique_lock<dxmt::mutex> lock(deps_mutex_);
        if (traits_.done(next))
          continue;
        parked_.insert({next, job});
        lock.unlock();
        {
          std::unique_lock<dxmt::mutex> wlock(worker_mutex_);
          if (speculative)
            speculative_running_--; // it resumes through admission again
          // a demanded job's demand reaches what it parked on
          if (auto it = nodes_.find(job); it != nodes_.end() && it->second.promoted)
            promoteRecord(&traits_.record(next));
        }
        worker_cond_.notify_all();
      }
      break;
    }
    running_.fetch_sub(1, std::memory_order_relaxed);
  }
}

} // namespace dxmt::compile
