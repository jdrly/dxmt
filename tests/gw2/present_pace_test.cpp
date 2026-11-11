// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Isrc/dxmt tests/gw2/present_pace_test.cpp -o $OUT
//
// gw2 (#157): DXMT_PRESENT_QUEUE=pace and its skip count (src/dxmt/dxmt_gw2_present_pace.hpp). No
// Metal, no Wine, no real time: every timestamp is made up.
//
// - skips: LiveSkips counts what the PRESENT section counts on a whole-refresh grid: a gap of two
//   refreshes at a target of one is one skipped refresh, `missed` when the later frame was encoded
//   before the earlier one reached the screen, `starved` when after; nothing across a dropped frame,
//   with no grid (vsyncs 0) or with the frame limiter.
// - sleep: a block answers once (blocked_seq), the sleep is the block less 2 ms, at most one refresh
//   less 2 ms, none for a block under the margin and none without a one-refresh grid.
// - backOff: a skip (missed or starved) while pace sleeps makes it give up at that step, once, with
//   the rise and the last sleep; it never sleeps again, whatever blocks.
// - notItsSkip: skips while pace hasn't slept for kWatchNs (an empty queue, a loading screen) don't
//   count; a later sleep starts from them.

#include "dxmt_gw2_present_pace.hpp"

#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

using dxmt::gw2::LiveSkips;
using dxmt::gw2::PresentPace;
using dxmt::gw2::skippedRefreshes;

constexpr int64_t kMs = 1'000'000;
constexpr double kRefreshMs = 1000.0 / 60;
constexpr int64_t kRefreshNs = 16'666'667;

static void
skips() {
  CHECK(skippedRefreshes(kRefreshMs, kRefreshMs, 1) == 0);
  CHECK(skippedRefreshes(2 * kRefreshMs, kRefreshMs, 1) == 1);
  CHECK(skippedRefreshes(2 * kRefreshMs, kRefreshMs, 2) == 0);
  CHECK(skippedRefreshes(0.4 * kRefreshMs, kRefreshMs, 1) == 0); // never below 0

  LiveSkips s;
  uint64_t t = 1000 * kMs;
  s.add(1, t - 40 * kMs, t, kRefreshMs, 1, 0);
  // frame 2 encoded before frame 1 was shown, shown two refreshes later: one missed
  s.add(2, t - 10 * kMs, t + 2 * kRefreshNs, kRefreshMs, 1, 0);
  CHECK(s.missed() == 1 && s.starved() == 0);
  t += 2 * kRefreshNs;
  // frame 3 encoded after frame 2 was shown, two refreshes later: one starved
  s.add(3, t + 1 * kMs, t + 2 * kRefreshNs, kRefreshMs, 1, 0);
  CHECK(s.missed() == 1 && s.starved() == 1);
  t += 2 * kRefreshNs;
  // on time: nothing
  s.add(4, t - 5 * kMs, t + kRefreshNs, kRefreshMs, 1, 0);
  CHECK(s.missed() == 1 && s.starved() == 1);
  t += kRefreshNs;
  // a dropped frame, then a late one: no pair across the drop
  s.add(5, t, 0, kRefreshMs, 1, 0);
  s.add(6, t - 5 * kMs, t + 3 * kRefreshNs, kRefreshMs, 1, 0);
  CHECK(s.missed() == 1 && s.starved() == 1);
  t += 3 * kRefreshNs;
  // no grid, or the frame limiter: nothing counted
  s.add(7, t - 5 * kMs, t + 3 * kRefreshNs, kRefreshMs, 0, 0);
  t += 3 * kRefreshNs;
  s.add(8, t - 5 * kMs, t + 3 * kRefreshNs, kRefreshMs, 1, 20.0);
  CHECK(s.missed() == 1 && s.starved() == 1);
}

static void
sleep() {
  PresentPace p;
  int64_t now = 1000 * kMs;
  // no block reported yet (seq 0)
  CHECK(p.next(now, 0, 0, kRefreshNs, 0, 0).sleep_ns == 0);
  // a 13 ms block: 11 ms, once
  CHECK(p.next(now, 13 * kMs, 1, kRefreshNs, 0, 0).sleep_ns == 11 * kMs);
  CHECK(p.next(now + kRefreshNs, 13 * kMs, 1, kRefreshNs, 0, 0).sleep_ns == 0);
  // a block longer than a refresh (a stall, not a full queue): at most a refresh less the margin
  CHECK(p.next(now + 2 * kRefreshNs, 40 * kMs, 2, kRefreshNs, 0, 0).sleep_ns == kRefreshNs - PresentPace::kMarginNs);
  // a block under the margin: no sleep
  CHECK(p.next(now + 3 * kRefreshNs, 1 * kMs, 3, kRefreshNs, 0, 0).sleep_ns == 0);
  CHECK(p.next(now + 4 * kRefreshNs, 2 * kMs, 4, kRefreshNs, 0, 0).sleep_ns == 0);
  // not one frame per refresh (refresh_ns 0): no sleep
  CHECK(p.next(now + 5 * kRefreshNs, 13 * kMs, 5, 0, 0, 0).sleep_ns == 0);
  CHECK(!p.gaveUp());
}

static void
backOff() {
  // a missed refresh while sleeping
  PresentPace p;
  int64_t now = 1000 * kMs;
  CHECK(p.next(now, 13 * kMs, 1, kRefreshNs, 5, 7).sleep_ns == 11 * kMs);
  now += kRefreshNs;
  CHECK(p.next(now, 12 * kMs, 2, kRefreshNs, 5, 7).sleep_ns == 10 * kMs);
  now += kRefreshNs;
  auto s = p.next(now, 13 * kMs, 3, kRefreshNs, 6, 7);
  CHECK(s.gave_up && s.sleep_ns == 0 && s.missed == 1 && s.starved == 0 && s.last_sleep_ns == 10 * kMs);
  CHECK(p.gaveUp());
  // never again, and the give-up is reported once
  for (int i = 1; i <= 100; i++) {
    auto t = p.next(now + i * kRefreshNs, 13 * kMs, 3 + i, kRefreshNs, 6, 7);
    CHECK(t.sleep_ns == 0 && !t.gave_up);
  }

  // a starved refresh (the sleep made the frame late) counts too, found a few presents later
  PresentPace q;
  now = 1000 * kMs;
  CHECK(q.next(now, 13 * kMs, 1, kRefreshNs, 0, 0).sleep_ns == 11 * kMs);
  now += 100 * kMs;
  CHECK(q.next(now, 1 * kMs, 2, kRefreshNs, 0, 0).sleep_ns == 0);
  now += 100 * kMs;
  s = q.next(now, 1 * kMs, 3, kRefreshNs, 0, 2);
  CHECK(s.gave_up && s.missed == 0 && s.starved == 2 && s.last_sleep_ns == 11 * kMs);
  CHECK(q.next(now + kRefreshNs, 13 * kMs, 4, kRefreshNs, 0, 2).sleep_ns == 0);
}

static void
notItsSkip() {
  PresentPace p;
  int64_t now = 1000 * kMs;
  // skips before pace ever slept
  CHECK(p.next(now, 0, 0, kRefreshNs, 3, 9).sleep_ns == 0);
  CHECK(!p.gaveUp());
  // it sleeps from there; the totals it started from aren't a rise
  CHECK(p.next(now + kRefreshNs, 13 * kMs, 1, kRefreshNs, 3, 9).sleep_ns == 11 * kMs);
  CHECK(!p.gaveUp());
  // the queue empties (no block to answer); after kWatchNs a skip isn't pace's
  int64_t later = now + kRefreshNs + PresentPace::kWatchNs;
  CHECK(p.next(later, 0, 1, kRefreshNs, 3, 12).sleep_ns == 0);
  CHECK(!p.gaveUp());
  // a skip just inside the window is
  CHECK(p.next(later + kRefreshNs, 13 * kMs, 2, kRefreshNs, 3, 12).sleep_ns == 11 * kMs);
  auto s = p.next(later + kRefreshNs + PresentPace::kWatchNs - 1, 0, 2, kRefreshNs, 4, 12);
  CHECK(s.gave_up && s.missed == 1 && s.starved == 0);
}

int
main() {
  skips();
  sleep();
  backOff();
  notItsSkip();
  if (failures) {
    std::printf("present_pace_test: %d failed\n", failures);
    return 1;
  }
  std::printf("present_pace_test: ok\n");
  return 0;
}
