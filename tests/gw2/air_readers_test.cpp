// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Isrc/dxmt tests/gw2/air_readers_test.cpp -o $OUT
//
// gw2 (#96 review): the startup line's DXMT_AIR_READERS reports the read connections actually open.
#include "dxmt_gw2_air_readers.hpp"
#include <cstdio>

using namespace dxmt::gw2;

static int failures = 0;
#define CHECK(cond)                                                                                                \
  do {                                                                                                             \
    if (!(cond)) {                                                                                                 \
      failures++;                                                                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                 \
    }                                                                                                              \
  } while (0)

int
main() {
  // DXMT_SHADER_CACHE=0: no readers, whatever DXMT_AIR_READERS asked for
  CHECK(airReadersOpen(false, false, 0) == 0);
  CHECK(airReadersOpen(false, true, 4) == 0);
  // the first reader couldn't be opened: no readers (the pool is never built without it)
  CHECK(airReadersOpen(true, false, 0) == 0);
  // the single reader (DXMT_AIR_READERS=1, or a pool that opened only its first connection)
  CHECK(airReadersOpen(true, true, 0) == 1);
  // a pool: its size (all opened, or fewer when some failed)
  CHECK(airReadersOpen(true, true, 4) == 4);
  CHECK(airReadersOpen(true, true, 3) == 3);
  // the line: the number, or "unopened" before any shader cache
  CHECK(airReadersEffective(0) == "0");
  CHECK(airReadersEffective(4) == "4");
  CHECK(airReadersEffective(kAirReadersUnopened) == "unopened");
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
