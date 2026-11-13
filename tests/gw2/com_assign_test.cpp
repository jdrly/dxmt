// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Itests/gw2/shim -Isrc/util/com tests/gw2/com_assign_test.cpp -o $OUT
//
// gw2 (#247): Com's raw pointer assignment (src/util/com/com_pointer.hpp) when the pointer is the one
// it already holds and it holds the last reference, the case SwapDeviceContextState reaches with
// state_object_. Until #247 it released first, so the object was destroyed and then referenced. The
// objects here count their references and mark themselves dead at zero without freeing their memory,
// so the old order shows as a reference taken on a dead object instead of a crash. For the public
// (AddRef/Release) and the private (AddRefPrivate/ReleasePrivate) flavour:
//
// - assigning a Com its own pointer while it holds the only reference keeps the object alive, with
//   one reference, and the Com's destructor then releases it;
// - assigning another object references the new one and releases the old one.

#include "com_pointer.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

struct Counted {
  unsigned pub = 0, prv = 0;
  bool dead = false;
  unsigned refs_while_dead = 0;

  void add(unsigned &n) {
    if (dead)
      refs_while_dead++;
    n++;
  }
  void release(unsigned &n) {
    n--;
    if (pub + prv == 0)
      dead = true; // where the real object deletes itself
  }
  unsigned long AddRef() { add(pub); return pub; }
  unsigned long Release() { release(pub); return pub; }
  void AddRefPrivate() { add(prv); }
  void ReleasePrivate() { release(prv); }
};

template <bool Public>
static void
check() {
  unsigned Counted::*count = Public ? &Counted::pub : &Counted::prv;
  {
    Counted a;
    {
      dxmt::Com<Counted, Public> com = &a;
      com = com.ptr();
      CHECK(!a.dead);
      CHECK(a.refs_while_dead == 0);
      CHECK(a.*count == 1);
    }
    CHECK(a.dead);
    CHECK(a.pub + a.prv == 0);
  }
  {
    Counted a, b;
    {
      dxmt::Com<Counted, Public> com = &a;
      com = &b;
      CHECK(a.dead);
      CHECK(!b.dead);
      CHECK(b.*count == 1);
    }
    CHECK(b.dead);
  }
}

int
main() {
  check<true>();
  check<false>();
  if (failures) {
    std::printf("com_assign_test: %d failures\n", failures);
    return 1;
  }
  std::printf("com_assign_test: ok\n");
  return 0;
}
