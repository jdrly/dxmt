// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/update_scope_test.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o $OUT
//
// gw2 (#145): host-side test of gw2::UpdateScope, the counting of an UpdateSubresource that
// DXMT_UPDATE_ONE_CALL handles without its Map and Unmap. MAP and PACE must count what the two calls'
// MapScopes and ApiScopes counted (the issue's acceptance: dcb.discard and dcb.unmap keep their counts
// with the switch on and off), and both lines must take the one timed interval. Simulated Win32 threads
// (shim/windows.h). Run it on the Mac:
//
//   clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/update_scope_test.cpp \
//     src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o /tmp/update_scope_test && /tmp/update_scope_test

#include "dxmt_gw2_map.hpp"
#include <windows.h>
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

  // Time: 1 in 16 timed and scaled by 16, so the estimate lands near the true total; MAP and PACE
  // take the same interval.
  {
    ApiThreadCache cache;
    MapCounters one;
    otherDrain();
    constexpr int kUpdates = 4096;
    constexpr int64_t kSpin = 2000;
    int64_t t0 = nowNs();
    for (int i = 0; i < kUpdates; i++) {
      UpdateScope u(one, MapKind::DefCb, MapOp::Discard, cache);
      spinNs(kSpin);
    }
    double wall = double(nowNs() - t0);
    uint64_t pace_ns = otherDrain().ns[(unsigned)Api::Map];
    uint64_t map_ns = one.ns[kDefCb][kDiscard];
    CHECK(pace_ns == map_ns);
    CHECK(map_ns > 0.6 * kUpdates * kSpin);
    CHECK(map_ns < 1.6 * wall);
    std::printf("timed estimate %.2f ms, spun %.2f ms, wall %.2f ms\n", map_ns / 1e6, kUpdates * kSpin / 1e6,
                wall / 1e6);
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("update_scope_test: ok\n");
  return 0;
}
