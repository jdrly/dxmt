// gw2 (#72, backport onto 3ea524a): the stop test against the old task_scheduler (dxmt_tasks.hpp).
// Not part of the DLL build; run it on the Mac (needs tests/gw2/shim/thread.hpp from #65, copied here):
//
//   clang++ -std=c++20 -O1 -Itests/gw2/shim -Isrc/dxmt -Isrc/util tests/gw2/task_scheduler_stop_test.cpp \
//     -o /tmp/task_scheduler_stop_test && /tmp/task_scheduler_stop_test
//
// It creates and destroys schedulers in a loop, the stop racing the workers' first wait, and fails if
// one stop takes longer than 5 s (a watchdog ends the process: a hung join can't be undone).

#include "dxmt_tasks.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

using namespace dxmt;

namespace dxmt::gw2 {
void compileQueue(int64_t) {}
void compileThreads(uint64_t) {}
void compileTraceSubmit(CompileTrace &) {}
} // namespace dxmt::gw2

struct Job {
  std::atomic<bool> done{false};
};

template <> struct dxmt::task_trait<Job *> {
  Job *run_task(Job *job) { return job; }
  bool get_done(Job *job) { return job->done.load(); }
  void set_done(Job *job) { job->done.store(true); }
  void on_submit(Job *) {}
};

int
main(int argc, char **argv) {
  const int cycles = argc > 1 ? atoi(argv[1]) : 20000;
  std::atomic<int> done{0};
  std::atomic<bool> finished{false};
  std::thread watchdog([&] {
    int last = -1;
    auto since = std::chrono::steady_clock::now();
    while (!finished.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      int now = done.load();
      if (now != last) {
        last = now;
        since = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() - since > std::chrono::seconds(5)) {
        std::printf("FAIL: a stop hung (cycle %d of %d): a worker missed the wakeup\n", now + 1, cycles);
        std::fflush(stdout);
        _exit(1);
      }
    }
  });
  for (int i = 0; i < cycles; i++) {
    {
      task_scheduler<Job *> scheduler; // 2 workers racing into their first wait
      if (i % 3 == 0) {
        Job job;
        scheduler.submit(&job);
        while (!job.done.load())
          std::this_thread::yield();
      }
    } // the stop
    done++;
  }
  finished = true;
  watchdog.join();
  std::printf("ok: %d stops, none hung\n", cycles);
  return 0;
}
