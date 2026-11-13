// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/update_scope_test.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o $OUT
//
// gw2 (#145): host-side test of gw2::UpdateScope, the counting of an UpdateSubresource that
// DXMT_UPDATE_ONE_CALL handles without its Map and Unmap. MAP and PACE must count what the two calls'
// MapScopes and ApiScopes counted (the issue's acceptance: dcb.discard and dcb.unmap keep their counts
// with the switch on and off), and both lines must take the one timed interval. gw2 (#150): a Map's
// MapScope takes its ApiScope's interval (two clock reads a timed call), and the clock's floor makes an
// empty interval read 0 on average. Simulated Win32 threads (shim/windows.h). Run it on the Mac:
//
//   clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/update_scope_test.cpp \
//     src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o /tmp/update_scope_test && /tmp/update_scope_test

#include "dxmt_gw2_map.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dxmt::gw2;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

constexpr unsigned kDefCb = (unsigned)MapKind::DefCb;
constexpr unsigned kDiscard = (unsigned)MapOp::Discard;
constexpr unsigned kNoOverwrite = (unsigned)MapOp::NoOverwrite;
constexpr unsigned kUnmap = (unsigned)MapOp::Unmap;
constexpr unsigned kMap = (unsigned)Api::Map;

// The nested path DXMT_UPDATE_ONE_CALL=0 keeps: UpdateSubresource1 calls Map, then Unmap, each with
// its own ApiScope and MapScope (d3d11_context_imm.cpp).
static void
nestedUpdate(MapCounters &c, ApiThreadCache &cache, MapOp op) {
  {
    ApiScope api(Api::Map, true, &cache);
    MapScope map(c, MapKind::DefCb, op, api);
  }
  {
    ApiScope api(Api::Map, true, &cache);
    MapScope map(c, MapKind::DefCb, MapOp::Unmap, api);
  }
}

static void
spinNs(int64_t ns) {
  int64_t t0 = nowNs();
  while (nowNs() - t0 < ns) {
  }
}

int
main() {
  fake::tid = 7; // never presents: PACE's calls and time go to the other threads' pool
  CHECK(statsEnabled());

  // Counts: the same buckets, and two PACE map calls an update, either way.
  {
    ApiThreadCache cache_a, cache_b;
    MapCounters nested, one;
    otherDrain();
    for (int i = 0; i < 1000; i++)
      nestedUpdate(nested, cache_a, i % 4 ? MapOp::Discard : MapOp::NoOverwrite);
    uint64_t nested_pace = otherDrain().calls[(unsigned)Api::Map];
    for (int i = 0; i < 1000; i++) {
      UpdateScope u(one, MapKind::DefCb, i % 4 ? MapOp::Discard : MapOp::NoOverwrite, cache_b);
    }
    uint64_t one_pace = otherDrain().calls[(unsigned)Api::Map];
    CHECK(std::memcmp(nested.calls, one.calls, sizeof(one.calls)) == 0);
    CHECK(one.calls[kDefCb][kDiscard] == 750);
    CHECK(one.calls[kDefCb][kNoOverwrite] == 250);
    CHECK(one.calls[kDefCb][kUnmap] == 1000);
    CHECK(nested_pace == 2000);
    CHECK(one_pace == 2000);
    CHECK(one.dirty);
    CHECK(one.ns[kDefCb][kUnmap] == 0); // the time goes on the Map's bucket
  }

  // Inside another entry point's scope, PACE leaves the update to the outer scope, as a nested
  // ApiScope does; MAP still counts it.
  {
    ApiThreadCache cache;
    MapCounters one;
    otherDrain();
    {
      ApiScope outer(Api::Flush, false, &cache);
      UpdateScope u(one, MapKind::DefCb, MapOp::Discard, cache);
    }
    ApiTotals t = otherDrain();
    CHECK(t.calls[(unsigned)Api::Map] == 0);
    CHECK(t.calls[(unsigned)Api::Flush] == 1);
    CHECK(one.calls[kDefCb][kDiscard] == 1);
    CHECK(one.calls[kDefCb][kUnmap] == 1);
    CHECK(apiThread().depth == 0);
  }

  // Time: 1 in 64 timed and scaled by 64 (#150); MAP and PACE take the same interval. Each timed
  // update is checked against the same clock, read right before and after it (#239): its interval,
  // less the floor, lies inside that bracket and holds the spin. A pause on the runner only widens
  // the bracket, so it can't fail this. Against the loop's wall time it could: a pause inside a timed
  // update counts 64 times in the estimate and once in the wall time. A scale applied twice or not at
  // all puts the timed updates outside (all 61 when I tried both; a pause can let one of them in).
  {
    ApiThreadCache cache;
    MapCounters one;
    otherDrain();
    constexpr int kUpdates = 4096;
    constexpr int64_t kSpin = 2000, kScale = 64;
    int64_t floor_ns = clockFloorNs();
    int timed = 0, outside = 0;
    for (int i = 0; i < kUpdates; i++) {
      int64_t before = one.ns[kDefCb][kDiscard], b0 = nowNs();
      {
        UpdateScope u(one, MapKind::DefCb, MapOp::Discard, cache);
        spinNs(kSpin);
      }
      int64_t bracket = nowNs() - b0, ns = one.ns[kDefCb][kDiscard] - before;
      if (ns) {
        timed++;
        outside += ns < kScale * (kSpin - floor_ns) || ns > kScale * bracket;
      }
    }
    int64_t pace_ns = otherDrain().ns[kMap];
    int64_t map_ns = one.ns[kDefCb][kDiscard];
    CHECK(pace_ns == map_ns);
    CHECK(outside == 0);
    CHECK(timed > kUpdates / kScale / 2 && timed < kUpdates / kScale * 2); // the thread's draws: the same every run
    std::printf("timed estimate %.2f ms: %d of %d updates timed, %d outside their bracket\n", map_ns / 1e6, timed,
                kUpdates, outside);
  }

  // gw2 (#150): the clock's floor is its own cost, and sinceNs takes it off once. The interval's end,
  // put back together from what sinceNs returned, lies between its start and a read taken right
  // after. That holds exactly on any machine. Without the floor taken off, or with it taken off
  // twice, it lands a floor past one end, which an empty interval of 0 or one clock step (100 ns)
  // misses most of the time.
  {
    int64_t floor_ns = clockFloorNs();
    CHECK(floor_ns > 0 && floor_ns < 1000);
    int outside = 0;
    for (int i = 0; i < 1000; i++) {
      int64_t t0 = nowNs();
      int64_t t1 = sinceNs(t0) + t0 + floor_ns;
      int64_t after = nowNs();
      outside += t1 < t0 || t1 > after;
    }
    CHECK(outside == 0);

    // On the real clock an empty interval reads 0 on average, but only while the clock costs what it
    // did when the floor was measured. On GitHub's virtual machines that cost moves by tens of ns
    // between the two (#179), so this bound only catches a floor that is wildly off. The median of 9
    // groups' means, so a preempted group doesn't count.
    double mean[9];
    for (auto &m : mean) {
      int64_t sum = 0;
      for (int i = 0; i < 1000; i++) {
        int64_t t0 = nowNs();
        sum += sinceNs(t0);
      }
      m = sum / 1000.0;
    }
    std::nth_element(mean, mean + 4, mean + 9);
    CHECK(std::fabs(mean[4]) < 1000);
    std::printf("clock floor %lld ns, empty interval %.2f ns, %d of 1000 outside\n", (long long)floor_ns, mean[4],
                outside);
  }

  // gw2 (#150): a buffer's Map, sampled 1 in 64: MAP takes the ApiScope's interval, so both lines read
  // the same time and a timed call reads the clock twice. Before, PACE and MAP each timed 1 in 16 with
  // their own two reads: kCalls / 4 reads.
  {
    ApiThreadCache cache;
    MapCounters c;
    otherDrain();
    clockFloorNs();
    constexpr int kCalls = 64 * 256;
    uint64_t reads0 = fake::qpc_reads;
    for (int i = 0; i < kCalls; i++) {
      ApiScope api(Api::Map, true, &cache);
      MapScope map(c, MapKind::DefCb, MapOp::Discard, api);
    }
    uint64_t reads = fake::qpc_reads - reads0;
    ApiTotals t = otherDrain();
    CHECK(t.calls[kMap] == kCalls);
    CHECK(c.calls[kDefCb][kDiscard] == kCalls);
    CHECK(t.ns[kMap] == c.ns[kDefCb][kDiscard]);
    CHECK(reads % 2 == 0);
    CHECK(reads > kCalls / 64 && reads < kCalls / 16); // about 2 x 256
    std::printf("buffer maps: %d calls, %llu clock reads\n", kCalls, (unsigned long long)reads);
  }

  // A texture's Map is timed on every call, at scale 1: on the ApiScope's interval when it times the
  // call, else on MAP's own. Two reads a call either way, and the total stays within the wall time
  // (the ApiScope's scale, 64, on its timed calls would double it).
  {
    ApiThreadCache cache;
    MapCounters c;
    otherDrain();
    constexpr int kCalls = 1024;
    uint64_t reads0 = fake::qpc_reads;
    for (int i = 0; i < kCalls; i++) {
      ApiScope api(Api::Map, true, &cache);
      MapScope map(c, MapKind::Tex, MapOp::Write, api);
    }
    CHECK(fake::qpc_reads - reads0 == 2 * kCalls);
    MapCounters spun;
    int64_t t0 = nowNs();
    for (int i = 0; i < kCalls; i++) {
      ApiScope api(Api::Map, true, &cache);
      MapScope map(spun, MapKind::Tex, MapOp::Write, api);
      spinNs(1000);
    }
    int64_t wall = nowNs() - t0;
    int64_t ns = spun.ns[(unsigned)MapKind::Tex][(unsigned)MapOp::Write];
    CHECK(ns > 0.8 * kCalls * 1000 && ns <= wall);
    otherDrain();
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("update_scope_test: ok\n");
  return 0;
}
