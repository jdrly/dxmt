// gw2 (B1, jdrly/dxmt #61): native smoke test of the bridge. It links the bridge's PE side
// (winemetal_thunks.c) and the real unix side (every source in winemetal_unix_src,
// src/winemetal/unix/meson.build) into one macOS executable, with DXMT_NATIVE: each bridge call goes
// straight to __wine_unix_call_funcs (src/nativemetal/wineunixlib.h) instead of through Wine. No
// Wine, no game, no window. A link failure here means a handler's source is missing from the shared
// list.
//
// The meson build (src/winemetal/unix/meson.build) builds it with the x86_64 Wine build; run it with
//
//   meson test -C <build>/x64 bridge-native-smoke      # or: <build>/x64/src/winemetal/unix/bridge_native_smoke
//
// It makes a bridge call into each unix source and checks what it can. With no Metal device it
// exits 77, which meson reports as skipped, never as passed.

#include "winemetal.h"
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

int
main(void) {
  // winemetal_unix.c: call numbers 10, 4, 3, 2, 7, 8 and 1
  obj_handle_t pool = NSAutoreleasePool_alloc_init();
  CHECK(pool != 0);
  obj_handle_t devices = WMTCopyAllDevices();
  uint64_t count = devices ? NSArray_count(devices) : 0;
  if (!count) {
    printf("SKIP: no Metal device\n");
    return 77;
  }
  obj_handle_t device = NSArray_object(devices, 0);
  CHECK(device != 0);
  char name[256] = {0};
  obj_handle_t ns_name = MTLDevice_name(device);
  CHECK(ns_name != 0);
  CHECK(NSString_getCString(ns_name, name, sizeof(name), WMTUTF8StringEncoding));
  CHECK(name[0] != '\0');
  printf("device: %s (%llu)\n", name, (unsigned long long)count);

  // cache.c: call number 152
  char cache_path[4096];
  uint32_t length = WMTGetMetalShaderCachePath(cache_path, sizeof(cache_path));
  CHECK(length == strlen(cache_path));
  printf("Metal shader cache: %s\n", length ? cache_path : "(none)");

  // pso_archive.c: call number 151; no archive was opened, so nothing is loaded
  struct WMTPSOArchiveStats stats;
  memset(&stats, 0xff, sizeof(stats));
  WMTPSOArchiveQuery(&stats);
  CHECK(stats.loaded == 0);

  // mtl4.c: call number 155. The PE side's default is WMTMetal4NoOS; on macOS 26 and later (the
  // build's deployment target) the unix side answers Supported or NoFamily.
  enum WMTMetal4Support m4 = MTLDevice_metal4Support(device);
  CHECK(m4 == WMTMetal4Supported || m4 == WMTMetal4NoFamily);
  printf("Metal 4 support: %u\n", (unsigned)m4);

  // mtl4fx.c: call number 190. Either answer is valid; reaching the handler (not a NULL table
  // entry) is the check.
  printf("Metal 4 MetalFX: %d\n", (int)MTLDevice_supportsMetal4FX(device));

  NSObject_release(devices);
  NSObject_release(pool);
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("bridge native smoke: OK\n");
  return 0;
}
