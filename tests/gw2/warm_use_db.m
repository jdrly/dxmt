// gw2 (#124): warm_use_test's access to a real recipe database through the bridge's unix side
// (src/winemetal/unix/cache.c: CacheWriter_set, CacheEnumerate), as plain C so the C++ test doesn't mix
// winemetal_thunks.h with the fork's headers. Not a test of its own.
#import <Foundation/Foundation.h>
#include <unistd.h>
#define WINEMETAL_API
#include "winemetal_thunks.h"
int _CacheWriter_alloc_init(void *);
int _CacheWriter_set(void *);
int _CacheEnumerate(void *);

static NSString *
cachePath(const char *path) {
  char dir[1024];
  confstr(_CS_DARWIN_USER_CACHE_DIR, dir, sizeof(dir));
  return [[NSString stringWithUTF8String:dir] stringByAppendingPathComponent:@(path)];
}

// Removes the database and its -wal, -shm and -lock files.
void
warmUseDbRemove(const char *path) {
  NSString *full = cachePath(path);
  for (NSString *suffix in @[ @"", @"-wal", @"-shm", @"-lock" ])
    [[NSFileManager defaultManager] removeItemAtPath:[full stringByAppendingString:suffix] error:nil];
}

// Writes rows through one CacheWriter (schema 1, as the recorder's). Returns the rows stored.
int
warmUseDbWrite(const char *path, int count, const void *const *keys, const unsigned *key_lengths,
               const void *const *values, const unsigned *value_lengths) {
  @autoreleasepool {
    struct unixcall_cache_alloc_init a = {0};
    a.path.ptr = path;
    a.version = 1;
    _CacheWriter_alloc_init(&a);
    if (!a.ret_cache)
      return 0;
    int stored = 0;
    for (int i = 0; i < count; i++) {
      dispatch_data_t d = dispatch_data_create(values[i], value_lengths[i], NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      struct unixcall_cache_set s = {0};
      s.cache = a.ret_cache;
      s.key.ptr = keys[i];
      s.key_length = key_lengths[i];
      s.value_data = (obj_handle_t)d;
      _CacheWriter_set(&s);
      dispatch_release(d);
      stored += s.ret_success ? 1 : 0;
    }
    [(NSObject *)a.ret_cache release];
    return stored;
  }
}

// One CacheEnumerate batch (bridge call 193), its result fields copied out.
void
warmUseDbEnumerate(const char *path, uint32_t prefix, uint64_t after, void *buffer, uint64_t capacity,
                   uint64_t *used, uint64_t *last_rowid, uint64_t *need, uint64_t *need_rowid, uint32_t *count,
                   uint32_t *status, uint32_t *error, uint32_t *busy, int *more) {
  struct unixcall_cache_enumerate p = {0};
  p.path.ptr = path;
  p.version = 1;
  p.after_rowid = after;
  p.buffer.ptr = buffer;
  p.capacity = capacity;
  p.prefix = prefix;
  _CacheEnumerate(&p);
  *used = p.ret_used;
  *last_rowid = p.ret_last_rowid;
  *need = p.ret_need;
  *need_rowid = p.ret_need_rowid;
  *count = p.ret_count;
  *status = p.ret_status;
  *error = p.ret_error;
  *busy = p.ret_busy;
  *more = p.ret_more;
}
