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
// - #114: a lock held past the retries (another connection's EXCLUSIVE transaction) is status 2 with
//   SQLITE_BUSY, not an empty table; one released after 30 ms is waited out (busy > 0, every row).
//   A row larger than the buffer says its size and rowid (need, need_rowid), so the caller can grow
//   the buffer or skip it, and the rows after it are still read.
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>
#include <sqlite3.h>
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

static void
writeRows(const char *path, int count, size_t (^length)(int)) {
  struct unixcall_cache_alloc_init a = {0};
  a.path.ptr = path;
  a.version = 1;
  _CacheWriter_alloc_init(&a);
  for (int i = 0; i < count; i++) {
    size_t len = length(i);
    uint8_t *value = calloc(1, len);
    memcpy(value, &i, sizeof(i));
    uint8_t key[21] = {'R'};
    memcpy(key + 1, &i, sizeof(i));
    dispatch_data_t d = dispatch_data_create(value, len, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
    struct unixcall_cache_set s = {0};
    s.cache = a.ret_cache;
    s.key.ptr = key;
    s.key_length = sizeof(key);
    s.value_data = (obj_handle_t)d;
    _CacheWriter_set(&s);
    dispatch_release(d);
    if (!s.ret_success)
      failures++;
  }
  [(NSObject *)a.ret_cache release];
}

static NSString *
cachePath(const char *path) {
  char dir[1024];
  confstr(_CS_DARWIN_USER_CACHE_DIR, dir, sizeof(dir));
  return [[NSString stringWithUTF8String:dir] stringByAppendingPathComponent:@(path)];
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

    // #114: SQLITE_BUSY. The writer's WAL file is switched to a rollback journal, where another
    // connection's EXCLUSIVE transaction keeps readers out (WAL readers are rarely refused)
    char busy_path[256];
    snprintf(busy_path, sizeof(busy_path), "dxmt/cache-enumerate-test-%d.exe/busy_1.db", getpid());
    writeRows(busy_path, 100, ^size_t(int i) { return 420; });
    sqlite3 *locker = NULL;
    sqlite3_open_v2(cachePath(busy_path).fileSystemRepresentation, &locker, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                    NULL);
    sqlite3_exec(locker, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
    bool locked = sqlite3_exec(locker, "BEGIN EXCLUSIVE;", NULL, NULL, NULL) == SQLITE_OK;
    t0 = ms();
    struct unixcall_cache_enumerate held = enumerate(busy_path, 'R', 0, buf, 1 << 20);
    double held_ms = ms() - t0;
    CHECK(locked && held.ret_status == 2 && held.ret_error == SQLITE_BUSY && held.ret_busy == 50 &&
              held.ret_count == 0 && !held.ret_more && held_ms < 2000,
          "a lock held past the retries: status %u, error %u (SQLITE_BUSY), %u retries in %.0f ms, not an empty table",
          held.ret_status, held.ret_error, held.ret_busy, held_ms);
    dispatch_semaphore_t committed = dispatch_semaphore_create(0);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC), dispatch_get_global_queue(0, 0), ^{
      sqlite3_exec(locker, "COMMIT;", NULL, NULL, NULL);
      dispatch_semaphore_signal(committed);
    });
    struct unixcall_cache_enumerate waited = enumerate(busy_path, 'R', 0, buf, 1 << 20);
    dispatch_semaphore_wait(committed, DISPATCH_TIME_FOREVER);
    dispatch_release(committed);
    sqlite3_close(locker);
    CHECK(waited.ret_status == 0 && waited.ret_busy > 0 && waited.ret_busy < 50 && waited.ret_count == 100 &&
              !waited.ret_more,
          "a lock released after 30 ms: waited out in %u retries, all %u rows", waited.ret_busy, waited.ret_count);

    // #114: a row larger than the buffer (2 MiB among 100-byte rows)
    char big_path[256];
    snprintf(big_path, sizeof(big_path), "dxmt/cache-enumerate-test-%d.exe/big_1.db", getpid());
    writeRows(big_path, 3, ^size_t(int i) { return i == 1 ? (size_t)2 << 20 : 100; });
    uint64_t big = 8 + 21 + (2 << 20);
    struct unixcall_cache_enumerate b1 = enumerate(big_path, 'R', 0, buf, 1 << 20);
    struct unixcall_cache_enumerate b2 = enumerate(big_path, 'R', b1.ret_last_rowid, buf, 1 << 20);
    CHECK(b1.ret_count == 1 && b1.ret_more && b1.ret_need == big && b1.ret_need_rowid == 2 && b2.ret_count == 0 &&
              b2.ret_more && b2.ret_need == big && b2.ret_need_rowid == 2 && b2.ret_status == 0,
          "a 2 MiB row in a 1 MiB buffer: 0 rows, more, need %llu B at rowid %llu (not the end)",
          (unsigned long long)b2.ret_need, (unsigned long long)b2.ret_need_rowid);
    void *grown = malloc(big);
    struct unixcall_cache_enumerate b3 = enumerate(big_path, 'R', b1.ret_last_rowid, grown, big);
    uint32_t vl = 0;
    memcpy(&vl, (uint8_t *)grown + 4 + 21, 4);
    struct unixcall_cache_enumerate b4 = enumerate(big_path, 'R', b2.ret_need_rowid, buf, 1 << 20);
    int number = -1;
    memcpy(&number, (uint8_t *)buf + 8 + 21, 4);
    CHECK(b3.ret_count == 1 && vl == 2 << 20 && b3.ret_more && b4.ret_count == 1 && !b4.ret_more && number == 2,
          "grown to it: the row is read; skipped past its rowid: the next row is read");
    free(grown);
    free(buf);

    // remove the scratch databases
    [[NSFileManager defaultManager] removeItemAtPath:[cachePath(path) stringByDeletingLastPathComponent] error:nil];
  }
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures != 0;
}
