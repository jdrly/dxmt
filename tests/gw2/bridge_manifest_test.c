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
//   B3 (#76): there it also dispatches by name through wmt_native_dispatch, the native build's
//   WINE_UNIX_CALL: old and new calls reach their handlers, reserved and out-of-range numbers return an
//   error. None of these calls needs a Metal device.
// The wow64 table exists only in the Wine build; the manifest generates it from the same lines.

#include "winemetal_thunks.h"
#include <stdio.h>
#include <string.h>

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
// gw2 (B3): as src/nativemetal/wineunixlib.h declares it (whose table declaration differs from the above)
int wmt_native_dispatch(enum WMTCall call, void *params);
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
  CHECK(WMTReserved_##number == number && number == position && sizeof(reserved_by) > 1);          \
  position++, reserved++;
#include "winemetal_calls.h"
  CHECK(position == WMTCall_Count);

  // Fixed anchors from before the manifest, and the numbers lanes were given
  CHECK(WMTCall_NSObject_retain == 0);
  CHECK(WMTCall_SM50Initialize == 74);
  CHECK(WMTReserved_83 == 83);
  CHECK(WMTCall_MTLCommandBuffer_presentDrawableWithDeadline == 146);
  CHECK(WMTCall_MetalView_visibility == 147);
  CHECK(WMTCall_MTLDevice_metal4Support == 155);
  CHECK(WMTCall_MTL4CommandEncoder_barrier == 178);
  CHECK(WMTReserved_179 == 179 && WMTReserved_189 == 189);
  CHECK(WMTCall_MTLDevice_supportsMetal4FX == 190);
  CHECK(WMTCall_CacheEnumerate == 193 && WMTReserved_194 == 194 && WMTReserved_199 == 199); // lane C (#64; 193: W2/W3)
  CHECK(WMTCall_WMTSetThreadBasePrioritySelf == 200); // #151
  CHECK(WMTCall_MTLResidencySet_allocationCount == 201); // #153
  CHECK(WMTCall_Count == 202);
  CHECK(calls == 184 && reserved == 18 && wow64 == 9);

#ifdef DXMT_NATIVE
  // Through the real table: a handler at every call number, NULL at every reserved one
#define WMT_CALL(number, name, handler) CHECK(__wine_unix_call_funcs[WMTCall_##name] != NULL);
#define WMT_CALL_WOW64(number, name, handler, wow64_handler) WMT_CALL(number, name, handler)
#define WMT_RESERVED(number, reserved_by) CHECK(__wine_unix_call_funcs[WMTReserved_##number] == NULL);
#include "winemetal_calls.h"
  printf("real table: checked %d entries\n", (int)WMTCall_Count);

  // gw2 (B3): dispatch by name through the real table. Old calls (upstream's numbers)...
  struct unixcall_generic_obj_ret pool = {0};
  CHECK(wmt_native_dispatch(WMTCall_NSAutoreleasePool_alloc_init, &pool) == 0 && pool.ret != 0);
  struct unixcall_get_os_version os = {0};
  CHECK(wmt_native_dispatch(WMTCall_WMTGetOSVersion, &os) == 0 && os.ret_major >= 26);
  // ... the fork's (#5, #46)...
  struct WMTPSOArchiveStats stats;
  memset(&stats, 0xff, sizeof(stats));
  struct unixcall_wmt_pso_archive_query query;
  WMT_MEMPTR_SET(query.stats, &stats);
  CHECK(wmt_native_dispatch(WMTCall_WMTPSOArchiveQuery, &query) == 0 && stats.loaded == 0);
  char cache_path[4096];
  struct unixcall_getmetalcachepath path = {.size = sizeof(cache_path), .ret_length = ~0ull};
  WMT_MEMPTR_SET(path.buf, cache_path);
  CHECK(wmt_native_dispatch(WMTCall_WMTGetMetalShaderCachePath, &path) == 0 && path.ret_length < sizeof(cache_path));
  CHECK(wmt_native_dispatch(WMTCall_NSObject_release, &pool.ret) == 0);
  // ... and holes: an error, where Wine's dispatcher would jump to NULL (each says so on stderr)
  CHECK(wmt_native_dispatch(WMTReserved_83, NULL) != 0);
  CHECK(wmt_native_dispatch(WMTReserved_179, NULL) != 0);
  CHECK(wmt_native_dispatch(WMTReserved_194, NULL) != 0);
  CHECK(wmt_native_dispatch(WMTCall_Count, NULL) != 0);
  printf("real table: dispatched 5 calls by name, 4 holes refused\n");
#endif

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("bridge manifest: %d calls (%d with their own wow64 handler), %d reserved: OK\n", calls, wow64,
         reserved);
  return 0;
}
