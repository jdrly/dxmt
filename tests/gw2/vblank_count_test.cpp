// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -DGW2_TEST_REAL_ENV -Itests/gw2/shim -Isrc/dxmt -Isrc/util -Isrc/winemetal tests/gw2/vblank_count_test.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o $OUT
// gw2-run: env -u DXMT_STATS "$OUT" on
// gw2-run: DXMT_STATS=0 "$OUT" off
//
// gw2 (#250): the count behind STATS' vblank (src/dxmt/dxmt_gw2_stats.hpp). IDXGIOutput::WaitForVBlank
// (src/dxgi/dxgi_output.cpp) is gw2::vblankCall() and S_OK; dxgi.h isn't built here, so the test calls
// vblankCall() as WaitForVBlank does. DXMT_STATS is read from the environment (GW2_TEST_REAL_ENV), so it
// runs twice:
//
// - on: 0 before the first call; 1, then 1,000 calls on one thread; then 4 threads at once, 100,000
//   calls each. Each time vblankCalls() reads back exactly the calls made.
// - off (DXMT_STATS=0): the same calls leave the count at 0.
//
// Run: tests/gw2/run.zsh pure, or the commands above with $OUT set.

#include "dxmt_gw2_stats.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace dxmt::gw2;

static int failures = 0;
#define CHECK_EQ(got, want)                                                                        \
  do {                                                                                             \
    uint64_t g_ = (got), w_ = (want);                                                              \
    if (g_ != w_) {                                                                                \
      std::printf("FAIL %s:%d: %s = %llu, want %llu\n", __FILE__, __LINE__, #got,                 \
                  (unsigned long long)g_, (unsigned long long)w_);                                 \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static void
calls(uint64_t n) {
  for (uint64_t i = 0; i < n; i++)
    vblankCall();
}

int
main(int argc, char **argv) {
  bool on = argc > 1 && std::strcmp(argv[1], "on") == 0;
  if (argc < 2 || (!on && std::strcmp(argv[1], "off") != 0)) {
    std::printf("usage: %s on|off\n", argv[0]);
    return 2;
  }
  if (statsEnabled() != on) {
    std::printf("FAIL statsEnabled() = %d for \"%s\"\n", statsEnabled(), argv[1]);
    return 1;
  }

  CHECK_EQ(vblankCalls(), 0);
  calls(1);
  CHECK_EQ(vblankCalls(), on ? 1 : 0);
  calls(1000);
  CHECK_EQ(vblankCalls(), on ? 1001 : 0);

  constexpr unsigned kThreads = 4;
  constexpr uint64_t kPerThread = 100000;
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < kThreads; t++)
    threads.emplace_back(calls, kPerThread);
  for (auto &t : threads)
    t.join();
  CHECK_EQ(vblankCalls(), on ? 1001 + kThreads * kPerThread : 0);

  std::printf("%s: %s\n", argv[1], failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
