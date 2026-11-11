// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Isrc/dxmt tests/gw2/layer_poll_test.cpp -o $OUT
//
// gw2 (#156): when the presenter asks the bridge for its layer's screen values
// (src/dxmt/dxmt_gw2_layer_poll.hpp). No Metal, no Wine.
//
// - everyPresent: with the interval at 0 every Present asks, as before #156, even at one clock reading.
// - interval: with 250 ms the first Present asks, then none until 250 ms after that ask, then the
//   next interval starts at that later ask, not at the first one's end.
// - resize: invalidate() makes the next Present ask within the interval, and the interval restarts
//   there.

#include "dxmt_gw2_layer_poll.hpp"

#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

using dxmt::gw2::LayerPropsPoll;

constexpr int64_t kMs = 1'000'000;

static void
everyPresent() {
  LayerPropsPoll poll(0);
  CHECK(poll.due(1000 * kMs));
  CHECK(poll.due(1000 * kMs));
  CHECK(poll.due(1000 * kMs + 1));
  int asks = 0;
  for (int i = 0; i < 100; i++)
    asks += poll.due(1000 * kMs + i * kMs);
  CHECK(asks == 100);
}

static void
interval() {
  LayerPropsPoll poll(250 * kMs);
  CHECK(poll.due(1000 * kMs));
  CHECK(!poll.due(1000 * kMs));
  CHECK(!poll.due(1249 * kMs));
  CHECK(!poll.due(1250 * kMs - 1));
  CHECK(poll.due(1250 * kMs));
  CHECK(!poll.due(1499 * kMs));
  // a Present 300 ms later (a stall): it asks, and the next interval starts there
  CHECK(poll.due(1800 * kMs));
  CHECK(!poll.due(2049 * kMs));
  CHECK(poll.due(2050 * kMs));

  // 60 fps for 2 s: 8 asks, one every 250 ms
  LayerPropsPoll frames(250 * kMs);
  int asks = 0;
  for (int i = 0; i < 120; i++)
    asks += frames.due(i * 16'666'667ll);
  CHECK(asks == 8);
}

static void
resize() {
  LayerPropsPoll poll(250 * kMs);
  CHECK(poll.due(1000 * kMs));
  CHECK(!poll.due(1100 * kMs));
  poll.invalidate();
  CHECK(poll.due(1100 * kMs));
  CHECK(!poll.due(1300 * kMs));
  CHECK(poll.due(1350 * kMs));
}

int
main() {
  everyPresent();
  interval();
  resize();
  if (failures) {
    std::printf("layer_poll_test: %d failed\n", failures);
    return 1;
  }
  std::printf("layer_poll_test: ok\n");
  return 0;
}
