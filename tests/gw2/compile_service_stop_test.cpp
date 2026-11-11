// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O1 -Itests/gw2/shim -Isrc/dxmt -Isrc/util tests/gw2/compile_service_stop_test.cpp -o $OUT
//
// gw2 (#72): regression test for the compile service's stop. ~Service sets `destroyed_` and wakes the
// workers; a worker that has just found its queues empty and is about to block must not miss that
// wakeup, or the destructor's join hangs (a device teardown, e.g. on game exit, that never returns).
//
// It creates and destroys services with idle workers in a loop, the stop racing the workers' first
// wait, and fails if one stop takes longer than 5 s (a watchdog ends the process: a hung join can't
// be undone). With the unlocked store the race hits within a few thousand cycles on an M5 Max.

#include "dxmt_compile_service.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

using namespace dxmt;

// the telemetry the service calls (production: dxmt_gw2_compile.cpp)
namespace dxmt::gw2 {
void compileQueue(int64_t) {}
void compileThreads(uint64_t) {}
bool compileTiming() { return false; }
int64_t compileNowNs() { return 0; }
CompileWaitScope::CompileWaitScope(const compile::JobRecord *) {}
CompileWaitScope::~CompileWaitScope() {}
void compilePromoted() {} // C2
void compileWorkerPriority(ThreadPriority) {} // #152
} // namespace dxmt::gw2

struct Job {
  compile::JobRecord record;
};

template <> struct dxmt::compile::JobTraits<Job *> {
  Job *run(Job *job) { return job; }
  bool done(Job *) { return true; }
  void setDone(Job *) {}
  compile::JobRecord &record(Job *job) { return job->record; }
  void deps(Job *, std::vector<Job *> &) {} // C2: none
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
      compile::Options o;
      o.initial_workers = 1 + i % 8; // workers racing into their first wait
      compile::Service<Job *> service(o);
      if (i % 3 == 0) {
        // and some that have run a job and are going back to wait
        Job job;
        service.submit(&job);
        while (job.record.state.load() != compile::JobState::Complete)
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
