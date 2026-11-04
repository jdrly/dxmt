// gw2-test: pure
// gw2-build: clang -std=c11 -Wall -Werror -Isrc/winemetal tests/gw2/bridge_manifest_test.c -o $OUT
// gw2-build: clang++ -std=c++20 -Wall -Werror -fsyntax-only -x c++ -Isrc/winemetal tests/gw2/bridge_manifest_test.c
// gw2-run: "$OUT"
// gw2-run: tests/gw2/bridge_manifest_reject.zsh "$T/bridge_manifest_reject"
//
// gw2 (B2, jdrly/dxmt #67): test of the bridge's call manifest (src/winemetal/winemetal_calls.h) and
// what it generates (enum WMTCall in winemetal_thunks.h, the two tables in winemetal_unix.c).
//
// Two builds of this file:
// - The X1 runner (tests/gw2/run.zsh pure) builds it alone, as C and as C++ (the manifest's checks
//   compile in both), and checks the enum against the manifest. bridge_manifest_reject.zsh then
//   builds mutated manifests and expects each to fail: a duplicate, a gap, a line out of order.
// - meson builds it as bridge-manifest (src/winemetal/unix/meson.build, suite gw2, never installed)
//   with the real unix side linked in and DXMT_NATIVE: it then also checks __wine_unix_call_funcs
//   itself, every call number with a handler and every reserved number NULL. Run it with
//   `tests/gw2/run.zsh pure --meson <build>/x64` or `meson test -C <build>/x64 bridge-manifest`.
// The wow64 table exists only in the Wine build; the manifest generates it from the same lines.

#include "winemetal_thunks.h"
#include <stdio.h>

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

#ifdef DXMT_NATIVE
extern const void *__wine_unix_call_funcs[]; // as winemetal_unix.c defines it
#endif

int
main(void) {
  int calls = 0, reserved = 0, wow64 = 0;

  // Every line at its number, counted the way the tables are generated
  int position = 0;
#define WMT_CALL(number, name, handler)                                                            \
  CHECK(WMTCall_##name == number && number == position);                                           \
  position++, calls++;
#define WMT_CALL_WOW64(number, name, handler, wow64_handler)                                       \
  WMT_CALL(number, name, handler)                                                                  \
  wow64++;
#define WMT_RESERVED(number, reserved_by)                                                          \
  CHECK(WMTCall_reserved_##number == number && number == position && sizeof(reserved_by) > 1);     \
  position++, reserved++;
#include "winemetal_calls.h"
  CHECK(position == WMTCall_Count);

  // Fixed anchors from before the manifest, and the numbers lanes were given
  CHECK(WMTCall_NSObject_retain == 0);
  CHECK(WMTCall_SM50Initialize == 74);
  CHECK(WMTCall_reserved_83 == 83);
  CHECK(WMTCall_MTLCommandBuffer_presentDrawableWithDeadline == 146);
  CHECK(WMTCall_MetalView_visibility == 147);
  CHECK(WMTCall_MTLDevice_metal4Support == 155);
  CHECK(WMTCall_MTL4CommandEncoder_barrier == 178);
  CHECK(WMTCall_reserved_179 == 179 && WMTCall_reserved_189 == 189);
  CHECK(WMTCall_MTLDevice_supportsMetal4FX == 190);
  CHECK(WMTCall_reserved_193 == 193 && WMTCall_reserved_199 == 199); // lane C (#64)
  CHECK(WMTCall_Count == 200);
  CHECK(calls == 181 && reserved == 19 && wow64 == 9);

#ifdef DXMT_NATIVE
  // Through the real table: a handler at every call number, NULL at every reserved one
#define WMT_CALL(number, name, handler) CHECK(__wine_unix_call_funcs[WMTCall_##name] != NULL);
#define WMT_CALL_WOW64(number, name, handler, wow64_handler) WMT_CALL(number, name, handler)
#define WMT_RESERVED(number, reserved_by) CHECK(__wine_unix_call_funcs[WMTCall_reserved_##number] == NULL);
#include "winemetal_calls.h"
  printf("real table: checked %d entries\n", (int)WMTCall_Count);
#endif

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("bridge manifest: %d calls (%d with their own wow64 handler), %d reserved: OK\n", calls, wow64,
         reserved);
  return 0;
}
