// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Isrc/dxmt tests/gw2/getdata_flush_test.cpp -o $OUT
//
// gw2 (#158): host-side test of gw2::getDataSkipsFlush, the rule DXMT_GETDATA_SKIP_FLUSH applies to a
// GetData without DONOTFLUSH that finds a timestamp or timestamp-disjoint query unresolved. The
// acceptance: such a poll still guarantees that the query's commands were submitted, so a poll loop
// always ends. The loop below polls a query of the chunk being recorded: the first poll must flush
// (commit the chunk), and only then may later polls skip. Run it on the Mac from the repository root:
//
//   clang++ -std=c++20 -Isrc/dxmt tests/gw2/getdata_flush_test.cpp -o /tmp/getdata_flush_test
//   /tmp/getdata_flush_test

#include "dxmt_gw2_timestamp.hpp"
#include <cstdio>
#include <initializer_list>

using namespace dxmt::gw2;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

int
main() {
  for (auto mode : {TimestampMode::Estimate, TimestampMode::Boundary}) {
    // the query's chunk is still being recorded: flush
    CHECK(!getDataSkipsFlush(true, mode, 7, 7));
    // committed: skip
    CHECK(getDataSkipsFlush(true, mode, 6, 7));
    // no chunk recorded (0): flush
    CHECK(!getDataSkipsFlush(true, mode, 0, 7));
    // the switch off: every poll flushes
    CHECK(!getDataSkipsFlush(false, mode, 6, 7));

    // a game that polls without DONOTFLUSH: the first poll commits the chunk, the rest skip
    uint64_t current = 3, issued = 3;
    unsigned flushes = 0;
    for (int poll = 0; poll < 100; poll++)
      if (!getDataSkipsFlush(true, mode, issued, current)) {
        flushes++;
        current++; // Flush commits the chunk (gw2_ts_uncommitted_)
      }
    CHECK(flushes == 1);
    CHECK(issued < current);
  }
  // exact: the query's value is its own command buffer's end, issued_chunk stays 0 (and a disjoint
  // query's value is an event id): every poll flushes, as upstream
  CHECK(!getDataSkipsFlush(true, TimestampMode::Exact, 6, 7));

  if (failures) {
    std::printf("getdata_flush_test: %d failures\n", failures);
    return 1;
  }
  std::printf("getdata_flush_test: ok\n");
  return 0;
}
