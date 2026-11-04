// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/api_thread_test.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o $OUT
//
// gw2 (#28): host-side test of the PACE scope's per-thread blocks (ApiThread, ApiThreadCache) with
// simulated Win32 threads (shim/windows.h). Not part of the DLL build; run it on the Mac:
//
//   clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/api_thread_test.cpp \
//     src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o /tmp/api_thread_test && /tmp/api_thread_test

#include "dxmt_gw2_stats.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace dxmt::gw2;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static uint64_t
otherMapCalls() {
  return otherDrain().calls[(unsigned)Api::Map];
}

// A Map with a staging wait inside it: an outer cached scope, an uncached lookup (waitAdd), and a
// nested cached scope, the path of the review's finding.
static void
mapWithWait(ApiThreadCache &ctx) {
  ApiScope outer(Api::Map, false, &ctx);
  waitAdd(Wait::Sync, 100);
  ApiScope nested(Api::Map, false, &ctx);
}

// Without a block in the fiber-local slot (FlsSetValue or FlsAlloc failed), a thread keeps one block
// for all its lookups, cached or not, and no cache holds it. Run in a child process: the fallback is
// a real thread_local, which the simulated threads share.
static int
fallbackCase(bool fail_alloc) {
  fake::fail_alloc = fail_alloc;
  fake::fail_set = !fail_alloc;
  if (fail_alloc)
    apiThreadsDetach(); // so the next lookup allocates the slot again, and fails
  ApiThreadCache ctx;
  fake::tid = 21;
  otherDrain();
  mapWithWait(ctx);
  ApiThread &t = apiThread();
  CHECK(&t == &apiThread()); // one block, not one per lookup
  CHECK(!t.cacheable);
  CHECK(t.depth == 0);
  CHECK(otherMapCalls() == 1);
  mapWithWait(ctx);
  CHECK(t.depth == 0);
  CHECK(otherMapCalls() == 1);
  // a later thread with the same id (the block has no exit callback) must not match it in the cache:
  // the cache never took it, so this lookup goes to apiThread again
  { ApiScope s(Api::Map, false, &ctx); }
  CHECK(t.depth == 0);
  return failures;
}

// gw2 (#121): the fallback block must never be one a cache can point at. Thread A (id 31) uses
// context X and exits, so X still points at A's released block. Thread B (id 32) then takes a block
// through another context while FlsSetValue (or FlsAlloc, after a FreeLibrary) fails, presents, and
// exits without an exit callback. Thread C reuses id 32 and enters X: the cache must not hand it B's
// block (B's presenter state). B runs on a real thread, so its fallback thread_local is its own and
// C's lookup on this thread can't find it that way; the threads run one after another.
static int
reclaimCase(bool fail_alloc) {
  ApiThreadCache x, y;
  fake::tid = 31;
  { ApiScope s(Api::Map, false, &x); }
  ApiThread *a_block = &apiThread();
  fake::exitThread();
  CHECK(a_block->tid.load() == 0);

  fake::fail_alloc = fail_alloc;
  fake::fail_set = !fail_alloc;
  if (fail_alloc)
    apiThreadsDetach(); // so the next lookup allocates the slot again, and fails
  ApiThread *b_block = nullptr;
  std::thread([&] {
    fake::tid = 32;
    { ApiScope s(Api::Map, false, &y); } // takes a block: A's is free to reclaim
    takeThreadInterval();                // B presents
    b_block = &apiThread();
    CHECK(b_block->presenter);
    CHECK(b_block != a_block); // a fallback block is fresh, never one a cache held
    fake::exitThread();        // no fiber-local value, so no exit callback: B's block keeps id 32
  }).join();
  CHECK(b_block->tid.load() == 32);

  fake::fail_alloc = false;
  fake::fail_set = false;
  fake::tid = 32;
  ApiThread &c = x.get();
  CHECK(&c != b_block);
  CHECK(!c.presenter);
  CHECK(c.depth == 0);
  return failures;
}

static void
runChild(const char *name, int (*test)(bool), bool fail_alloc) {
  std::fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    int failed = test(fail_alloc);
    std::fflush(stdout); // _Exit doesn't flush: keep the child's FAIL lines
    std::_Exit(failed ? 1 : 0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::printf("FAIL %s case\n", name);
    failures++;
  }
}

int
main() {
  ApiThreadCache ctx;

  // Thread A (id 5) presents, uses the context, and exits.
  fake::tid = 5;
  takeThreadInterval();
  { ApiScope s(Api::Map, false, &ctx); }
  ThreadInterval a = takeThreadInterval();
  CHECK(a.api.calls[(unsigned)Api::Map] == 1);
  ApiThread *a_block = &apiThread();
  fake::exitThread();
  CHECK(a_block->tid.load() == 0); // released: no cache returns it

  // Thread C reuses id 5. Its first entry is a cache hit on A's old block's id, followed by an
  // uncached lookup inside the scope. It must not see A's presenter state, and the block it uses
  // must not be reset under the open scope.
  fake::tid = 5;
  otherDrain();
  mapWithWait(ctx);
  ApiThread &c = apiThread();
  CHECK(c.depth == 0);
  CHECK(!c.presenter);                  // C never presented: its calls are the other threads'
  CHECK(otherMapCalls() == 1);          // the outer scope counts once, the nested one not at all
  mapWithWait(ctx);
  CHECK(c.depth == 0);
  CHECK(otherMapCalls() == 1);          // the next call is an outer scope again (no stale depth)
  CHECK(c.interval.wait_count[(unsigned)Wait::Sync] == 0); // waits only count for presenters

  // Two live threads alternating on one context keep their own blocks.
  fake::tid = 7;
  takeThreadInterval(); // 7 presents
  { ApiScope s(Api::Map, false, &ctx); }
  fake::tid = 5;
  { ApiScope s(Api::Map, false, &ctx); }
  fake::tid = 7;
  { ApiScope s(Api::Map, false, &ctx); }
  CHECK(takeThreadInterval().api.calls[(unsigned)Api::Map] == 2);
  CHECK(otherMapCalls() == 1);

  // FreeLibrary: freeing the slot releases every thread's block.
  ApiThread *b5 = &(fake::tid = 5, apiThread());
  apiThreadsDetach();
  CHECK(b5->tid.load() == 0);

  runChild("FlsSetValue", fallbackCase, false);
  runChild("FlsAlloc", fallbackCase, true);
  runChild("FlsSetValue after a reclaim", reclaimCase, false);
  runChild("FlsAlloc after a reclaim", reclaimCase, true);

  std::printf(failures ? "%d failure(s)\n" : "ok\n", failures);
  return failures ? 1 : 0;
}
