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
// Not yet (C2, `DXMT_COMPILE_DEMAND`, off): owning jobs, dependencies declared at creation, a demand
// lane and speculative admission. Stable keys and shared results come with C3/C4.

#include "dxmt_gw2_compile.hpp"
#include "thread.hpp"
#include "util_win32_compat.h"
#include <atomic>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

namespace dxmt::compile {

enum class JobState : uint8_t { Absent = 0, Queued, Running, Parked, Building, Complete };

// A job's record, owned by the service (the job object embeds it). State and `parked_on` are always
// kept; the timeline only when STATS was on at submit (`timed`), as before (#46): a job submitted with
// STATS off isn't classified when it is demanded.
struct JobRecord {
  std::atomic<JobState> state{JobState::Absent};
  std::atomic<const JobRecord *> parked_on{nullptr}; // the dependency it was last parked on
  std::atomic<bool> timed{false};
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
inline void
demand(const JobRecord &r, std::atomic_bool &ready) {
  if (!ready.load(std::memory_order_acquire)) {
    gw2::CompileWaitScope wait(&r);
    ready.wait(false, std::memory_order_acquire);
  }
}

// How the service drives a job type: run it (it returns itself when done, else the dependency it
// waits on), read and set its done flag, and reach its record.
template <typename Job> struct JobTraits {
  Job run(Job job);
  bool done(Job job);
  void setDone(Job job);
  JobRecord &record(Job job);
};

struct Options {
  unsigned initial_workers = 2;
  unsigned max_workers = 0; // 0: 2 x CPUs
  int priority = THREAD_PRIORITY_TIME_CRITICAL;
};

template <typename Job, typename Traits = JobTraits<Job>> class Service {
public:
  explicit Service(Options options = {});
  ~Service(); // stop: workers finish the job they run, nothing else; queued jobs are dropped (as before)

  // Queues a new job (legacy: FIFO, never blocks on a build). C2/C4 add acquire(key, lane, deps, job).
  void submit(Job job);

  uint64_t
  runningWorkers() {
    return running_.load(std::memory_order_relaxed);
  }

private:
  void workerLoop();

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
  destroyed_.store(true);
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
Service<Job, Traits>::submit(Job job) {
  detail::onSubmit(Traits{}.record(job)); // before any worker can take it
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

} // namespace dxmt::compile
