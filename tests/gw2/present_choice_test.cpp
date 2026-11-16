// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Itests/gw2/shim -Isrc/dxmt -Isrc/winemetal tests/gw2/present_choice_test.cpp -o $OUT
//
// gw2 (#283): gw2::presentChoice and gw2::presentThrough (src/dxmt/dxmt_gw2_present_choice.hpp), the
// kind of present the Metal 4 branch of the context's present case picks once. No Metal, no Wine.
//
// - choice: a minimum duration wins over a deadline, a deadline over a plain present, and a value
//   that isn't above 0 doesn't count, as the context's if/else chain had it.
// - through: each choice calls exactly one of a Metal 3 command buffer's three presents, with its value.
//
// Run it from the checkout's root (or through tests/gw2/run.zsh).

#include "dxmt_gw2_present_choice.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

using dxmt::gw2::presentChoice;
using dxmt::gw2::presentThrough;

// stands in for WMT::CommandBuffer: counts which present was called, and with what
struct FakeBuffer {
  int after = 0, deadline = 0, plain = 0;
  int drawable = 0;
  double value = -1;
  void
  presentDrawableAfterMinimumDuration(int d, double v) {
    after++, drawable = d, value = v;
  }
  void
  presentDrawableWithDeadline(int d, double v) {
    deadline++, drawable = d, value = v;
  }
  void
  presentDrawable(int d) {
    plain++, drawable = d, value = 0;
  }
};

static void
choice() {
  auto c = presentChoice(0.016, 0.008);
  CHECK(c.mode == WMTDrawablePresentAfterMinimumDuration && c.value == 0.016);
  c = presentChoice(0, 0.008);
  CHECK(c.mode == WMTDrawablePresentWithDeadline && c.value == 0.008);
  c = presentChoice(0, 0);
  CHECK(c.mode == WMTDrawablePresent && c.value == 0);
  c = presentChoice(-1, -1);
  CHECK(c.mode == WMTDrawablePresent && c.value == 0);
  c = presentChoice(-1, 0.008);
  CHECK(c.mode == WMTDrawablePresentWithDeadline && c.value == 0.008);
}

static void
through() {
  FakeBuffer a;
  presentThrough(a, 7, presentChoice(0.016, 0.008));
  CHECK(a.after == 1 && a.deadline == 0 && a.plain == 0 && a.drawable == 7 && a.value == 0.016);
  FakeBuffer d;
  presentThrough(d, 8, presentChoice(0, 0.008));
  CHECK(d.after == 0 && d.deadline == 1 && d.plain == 0 && d.drawable == 8 && d.value == 0.008);
  FakeBuffer p;
  presentThrough(p, 9, presentChoice(0, 0));
  CHECK(p.after == 0 && p.deadline == 0 && p.plain == 1 && p.drawable == 9 && p.value == 0);
}

int
main() {
  choice();
  through();
  if (failures) {
    std::printf("present_choice_test: %d failed\n", failures);
    return 1;
  }
  std::printf("present_choice_test: ok\n");
  return 0;
}
