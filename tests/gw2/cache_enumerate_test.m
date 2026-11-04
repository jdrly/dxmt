// gw2-test: pure
// gw2-build: clang -x objective-c -std=gnu17 -fno-objc-arc -O1 -DDXMT_NO_PRIVATE_API -Isrc/winemetal -Isrc/winemetal/unix src/winemetal/unix/cache.c tests/gw2/cache_enumerate_test.m -framework Foundation -lsqlite3 -o $OUT
//
// gw2 (W2/W3): native test of bridge call 193, CacheEnumerate (src/winemetal/unix/cache.c), through its
// unix entry point, on a scratch database in the user cache dir (removed afterwards). No Metal.
//
// - A session's worth of rows (10,000 recipes of 420 B among 3,000 DXBC rows of 5 KB) written by the
//   bridge's CacheWriter come back by prefix, in first-write (rowid) order, each once, across batches.
// - A small buffer splits a batch (`more`), and the next call continues after the last row.
// - Prefix 256 reads every row; a missing database reads nothing (status 1).
// - The warm thread's read (all recipes, 1 MB batches) is timed: it bounds warming's startup cost.
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>
#include <unistd.h>
#define WINEMETAL_API
#include "winemetal_thunks.h"
int _CacheWriter_alloc_init(void *);
int _CacheWriter_set(void *);
int _CacheEnumerate(void *);

static int failures = 0;
#define CHECK(c, ...)                                                                                              \
  do {                                                                                                             \
    if (!(c)) {                                                                                                    \
      failures++;                                                                                                  \
      printf("FAIL: ");                                                                                            \
    } else                                                                                                         \
      printf("ok:   ");                                                                                            \
    printf(__VA_ARGS__);                                                                                           \
    printf("\n");                                                                                                  \
  } while (0)

static double
ms(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom)
    mach_timebase_info(&tb);
  return mach_absolute_time() * (double)tb.numer / tb.denom / 1e6;
}

static struct unixcall_cache_enumerate
enumerate(const char *path, uint32_t prefix, uint64_t after, void *buf, uint64_t cap) {
  struct unixcall_cache_enumerate p = {0};
  p.path.ptr = path;
  p.version = 1;
  p.after_rowid = after;
  p.buffer.ptr = buf;
  p.capacity = cap;
  p.prefix = prefix;
  _CacheEnumerate(&p);
  return p;
}

int
main(void) {
  @autoreleasepool {
    char path[256];
    snprintf(path, sizeof(path), "dxmt/cache-enumerate-test-%d.exe/recipes_1.db", getpid());
    struct unixcall_cache_alloc_init a = {0};
    a.path.ptr = path;
    a.version = 1;
    _CacheWriter_alloc_init(&a);
    CHECK(a.ret_cache, "writer opened %s", path);
    uint8_t value[5000];
    for (int i = 0; i < (int)sizeof(value); i++)
      value[i] = (uint8_t)(i * 7);
    int recipes = 0, dxbc = 0;
    for (int i = 0; i < 13000; i++) {
      bool is_recipe = i % 13 >= 3; // 10 of every 13 rows
      uint8_t key[21] = {is_recipe ? 'R' : 'D'};
      memcpy(key + 1, &i, sizeof(i));
      size_t len = is_recipe ? 420 : 5000;
      memcpy(value, &i, sizeof(i)); // the row's own number, to check order
      dispatch_data_t d = dispatch_data_create(value, len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      struct unixcall_cache_set s = {0};
      s.cache = a.ret_cache;
      s.key.ptr = key;
      s.key_length = sizeof(key);
      s.value_data = (obj_handle_t)d;
      _CacheWriter_set(&s);
      dispatch_release(d);
      if (!s.ret_success)
        failures++;
      is_recipe ? recipes++ : dxbc++;
    }
    [(NSObject *)a.ret_cache release];

    // all recipes, 1 MB batches, as the warm thread reads them
    void *buf = malloc(1 << 20);
    double t0 = ms();
    uint64_t after = 0;
    int got = 0, batches = 0, last_number = -1;
    bool ordered = true, prefixed = true, more = true;
    while (more) {
      struct unixcall_cache_enumerate e = enumerate(path, 'R', after, buf, 1 << 20);
      batches++;
      uint8_t *p = buf;
      for (uint32_t i = 0; i < e.ret_count; i++) {
        uint32_t kl, vl;
        memcpy(&kl, p, 4);
        prefixed &= p[4] == 'R';
        memcpy(&vl, p + 4 + kl, 4);
        int number;
        memcpy(&number, p + 8 + kl, 4);
        ordered &= number > last_number && vl == 420;
        last_number = number;
        p += 8 + kl + vl;
        got++;
      }
      more = e.ret_more;
      after = e.ret_last_rowid;
      if (!e.ret_count)
        break;
    }
    double read_ms = ms() - t0;
    CHECK(got == recipes && ordered && prefixed, "%d of %d recipes back, each once, in write order, only R rows",
          got, recipes);
    CHECK(batches > 1, "read in %d batches of 1 MB: %.1f ms for %d recipes (the warm thread's startup read)",
          batches, read_ms, got);

    // a small buffer: a split batch continues where it stopped
    struct unixcall_cache_enumerate e1 = enumerate(path, 'R', 0, buf, 3 * (8 + 21 + 420) + 10);
    struct unixcall_cache_enumerate e2 = enumerate(path, 'R', e1.ret_last_rowid, buf, 3 * (8 + 21 + 420) + 10);
    int first_of_second;
    memcpy(&first_of_second, (uint8_t *)buf + 8 + 21, 4);
    CHECK(e1.ret_count == 3 && e1.ret_more && e2.ret_count == 3 && first_of_second == 6,
          "a small buffer: 3 rows and more, then the next 3 (row 6 first)");

    // every row, and a missing database
    int all = 0;
    after = 0;
    more = true;
    while (more) {
      struct unixcall_cache_enumerate e = enumerate(path, 256, after, buf, 1 << 20);
      all += e.ret_count;
      more = e.ret_more;
      after = e.ret_last_rowid;
      if (!e.ret_count)
        break;
    }
    CHECK(all == recipes + dxbc, "prefix 256: all %d rows", all);
    struct unixcall_cache_enumerate none = enumerate("dxmt/no-such-exe/recipes_1.db", 'R', 0, buf, 1 << 20);
    CHECK(none.ret_status == 1 && none.ret_count == 0 && !none.ret_more, "a missing database: nothing, status 1");
    free(buf);

    // remove the scratch database
    char dir[1024];
    confstr(_CS_DARWIN_USER_CACHE_DIR, dir, sizeof(dir));
    NSString *db = [[NSString stringWithUTF8String:dir] stringByAppendingPathComponent:@(path)];
    [[NSFileManager defaultManager] removeItemAtPath:[db stringByDeletingLastPathComponent] error:nil];
  }
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures != 0;
}
