// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Isrc/dxmt tests/gw2/cadence_test.cpp -o $OUT
//
// gw2 (#314): DXMT_STEADY_SLEEP's deadline arithmetic (src/dxmt/dxmt_gw2_cadence.hpp) against the
// wall-clock code it stands in for. The swap chain needs the PE toolchain, so the test replays
// d3d11_swapchain.cpp's LimitFrameRate and ThrottleBackground on plain integers (ns) as the oracle,
// with the time a sleep ends standing in for sleep_until's deadline. No real time is read: `now`
// moves by the frame's work plus what the step asked to sleep.

#include "dxmt_gw2_cadence.hpp"
#include <cstdint>
#include <cstdio>
#include <random>

using dxmt::gw2::Cadence;

static int failures = 0;
#define CHECK(...)                                                                                 \
  do {                                                                                             \
    if (!(__VA_ARGS__)) {                                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

constexpr int64_t kMs = 1'000'000;

// LimitFrameRate's wall-clock steps, as they read before #314: a deadline of 0 is
// clock::time_point{}, "not armed". Returns the sleep sleep_until(cap_deadline_) makes.
struct OldLimiter {
  int64_t deadline = 0;
  int64_t
  step(int64_t now, int64_t interval) {
    if (interval <= 0 || deadline == 0) {
      deadline = interval > 0 ? now : 0;
      return 0;
    }
    deadline += interval;
    if (deadline <= now) {
      deadline = now;
      return 0;
    }
    return deadline - now;
  }
};

// ThrottleBackground's: a change of fps or reason sets the deadline to now; with fps, the deadline
// moves on by 1/fps and Present sleeps to it, or a late frame restarts it.
struct OldThrottle {
  int64_t deadline = 0;
  int64_t
  step(int64_t now, uint32_t fps, bool changed) {
    if (changed)
      deadline = now;
    if (!fps)
      return 0;
    deadline += 1000000000ll / fps;
    if (deadline <= now) {
      deadline = now;
      return 0;
    }
    return deadline - now;
  }
};

// The swap chain's steady path of ThrottleBackground.
static int64_t
throttleStep(Cadence &c, int64_t now, uint32_t fps, bool changed) {
  if (changed)
    c.restart(now);
  return fps ? c.step(now, 1000000000ll / fps) : 0;
}

static void
cases() {
  // 1. The first call with an interval arms and returns 0.
  {
    Cadence c;
    OldLimiter o;
    CHECK(c.step(100 * kMs, 20 * kMs) == 0);
    CHECK(o.step(100 * kMs, 20 * kMs) == 0);
  }
  // 2. A frame that takes 10 ms of a 20 ms interval sleeps the other 10 ms, every frame.
  {
    Cadence c;
    OldLimiter o;
    int64_t now = 100 * kMs;
    c.step(now, 20 * kMs);
    o.step(now, 20 * kMs);
    for (int i = 0; i < 10; i++) {
      now += 10 * kMs;
      int64_t s = c.step(now, 20 * kMs);
      CHECK(s == 10 * kMs);
      CHECK(s == o.step(now, 20 * kMs));
      now += s;
    }
  }
  // 3. An oversleep of 3 ms comes back: the next sleep is 3 ms shorter.
  {
    Cadence c;
    int64_t now = 100 * kMs;
    c.step(now, 20 * kMs);
    now += 10 * kMs;
    int64_t s = c.step(now, 20 * kMs);
    CHECK(s == 10 * kMs);
    now += s + 3 * kMs;
    now += 10 * kMs;
    CHECK(c.step(now, 20 * kMs) == 7 * kMs);
  }
  // 4. A frame that overran (now past the deadline) returns 0 and restarts from now.
  {
    Cadence c;
    int64_t now = 100 * kMs;
    c.step(now, 20 * kMs);
    now += 35 * kMs;
    CHECK(c.step(now, 20 * kMs) == 0);
    now += 5 * kMs;
    CHECK(c.step(now, 20 * kMs) == 15 * kMs);
    // A frame that ends exactly on its deadline doesn't sleep either, in both.
    OldLimiter o;
    o.step(now, 20 * kMs);
    Cadence d;
    d.step(now, 20 * kMs);
    now += 20 * kMs;
    CHECK(d.step(now, 20 * kMs) == 0);
    CHECK(o.step(now, 20 * kMs) == 0);
    now += 1;
    CHECK(d.step(now, 20 * kMs) == 20 * kMs - 1);
    CHECK(o.step(now, 20 * kMs) == 20 * kMs - 1);
  }
  // 5. interval <= 0 disarms, and the next call with an interval only arms.
  {
    Cadence c;
    int64_t now = 100 * kMs;
    c.step(now, 20 * kMs);
    now += 5 * kMs;
    CHECK(c.step(now, 0) == 0);
    now += 5 * kMs;
    CHECK(c.step(now, -1) == 0);
    now += 5 * kMs;
    CHECK(c.step(now, 20 * kMs) == 0);
    now += 5 * kMs;
    CHECK(c.step(now, 20 * kMs) == 15 * kMs);
  }
  // 6. restart(now) then step(now, interval) returns the interval: the throttle's first frame.
  {
    Cadence c;
    OldThrottle o;
    int64_t now = 100 * kMs;
    c.restart(now);
    CHECK(c.step(now, 1000000000ll / 30) == 1000000000ll / 30);
    CHECK(o.step(now, 30, true) == 1000000000ll / 30);
  }
}

// 7. The oracles and Cadence agree over 1,000 frames each: random work (0 to 40 ms), random
// oversleep (0 to 2 ms), and the interval or the throttle's state changing now and then. mt19937's
// raw output is the same on every platform; the standard's distributions aren't.
static void
randomized() {
  std::mt19937 rng(314);
  auto pick = [&](uint32_t n) { return uint32_t(rng() % n); };

  const int64_t intervals[] = {0, 8'333'333, 16'666'666, 20 * kMs, 33'333'333, -1};
  {
    Cadence c;
    OldLimiter o;
    int64_t now = 1000 * kMs, interval = intervals[3];
    int mismatches = 0, slept = 0, overran = 0;
    for (int i = 0; i < 1000; i++) {
      if (pick(20) == 0)
        interval = intervals[pick(6)];
      now += int64_t(pick(40'000'001));
      int64_t s = c.step(now, interval);
      if (s != o.step(now, interval) || s < 0)
        mismatches++;
      (s ? slept : overran)++;
      now += s + int64_t(pick(2'000'001));
    }
    CHECK(mismatches == 0);
    std::printf("cadence_test: %d slept, %d didn't\n", slept, overran);
    CHECK(slept > 100 && overran > 100); // both branches, many times
  }

  const uint32_t rates[] = {0, 5, 10, 30};
  {
    Cadence c;
    OldThrottle o;
    int64_t now = 1000 * kMs;
    uint32_t fps = 0, reason = 0;
    int mismatches = 0, slept = 0, overran = 0;
    for (int i = 0; i < 1000; i++) {
      uint32_t new_fps = fps, new_reason = reason;
      if (pick(25) == 0)
        new_fps = rates[pick(4)];
      if (pick(50) == 0)
        new_reason = pick(3); // Foreground, NoForeground, Occluded at the same fps
      bool changed = new_fps != fps || new_reason != reason;
      fps = new_fps;
      reason = new_reason;
      now += int64_t(pick(250'000'001));
      int64_t s = throttleStep(c, now, fps, changed);
      if (s != o.step(now, fps, changed) || s < 0)
        mismatches++;
      (s ? slept : overran)++;
      now += s + int64_t(pick(2'000'001));
    }
    CHECK(mismatches == 0);
    std::printf("cadence_test: %d slept, %d didn't\n", slept, overran);
    CHECK(slept > 100 && overran > 100); // both branches, many times
  }
}

int
main() {
  cases();
  randomized();
  if (failures) {
    std::printf("cadence_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("cadence_test: passed\n");
  return 0;
}
