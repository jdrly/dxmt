#import <Foundation/Foundation.h>
#include <string.h>
#include "sqlite3.h"
#define WINEMETAL_API
#include "../winemetal_thunks.h"

@interface CacheReader : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (dispatch_data_t)get:(NSData *)key;
@end

@interface CacheWriter : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (BOOL)set:(NSData *)key value:(dispatch_data_t)value;
- (uint64_t)byteSize;
@end

@interface CacheReader () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt;
}
@end

static inline NSString *
resolve_cache_dir(NSString *path, bool path_is_file) {
  if (![path hasPrefix:@"/"]) {
    char buf[PATH_MAX];
    size_t len = confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf));

    if (!len) {
      return nil;
    }

    NSString *base = [NSString stringWithUTF8String:buf];
    path = [base stringByAppendingPathComponent:path];
  }
  [[NSFileManager defaultManager] createDirectoryAtPath:path_is_file ? [path stringByDeletingLastPathComponent] : path
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  return path;
}

@implementation CacheReader

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheReader] Failed to resolve cache path");
      return nil;
    }
    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    if (sqlite3_open_v2([dbPath fileSystemRepresentation], &_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) !=
        SQLITE_OK) {
      NSLog(@"[CacheReader] Failed to open DB: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }

    NSString *sqlGet = [NSString stringWithFormat:@"SELECT value FROM %@ WHERE key = ?;", tableName];
    if (sqlite3_prepare_v2(_db, sqlGet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheReader] Failed to prepare SELECT: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }
  }
  return self;
}

- (dispatch_data_t)get:(NSData *)key {
  sqlite3_reset(_stmt);
  sqlite3_clear_bindings(_stmt);
  sqlite3_bind_blob64(_stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  dispatch_data_t result = nil;
  if (sqlite3_step(_stmt) == SQLITE_ROW) {
    const void *bytes = sqlite3_column_blob(_stmt, 0);
    int len = sqlite3_column_bytes(_stmt, 0);
    result = dispatch_data_create(bytes, len, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  }
  sqlite3_reset(_stmt);
  return result;
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_db)
    sqlite3_close(_db);
  [super dealloc];
}

@end

@interface CacheWriter () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt;
}
@end

@implementation CacheWriter

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheWriter] Failed to resolve cache path");
      return nil;
    }

    NSString *lockPath = [dbPath stringByAppendingString:@"-lock"];
    int fd = open([lockPath fileSystemRepresentation], O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      NSLog(@"[CacheWriter] Failed to open file for locking %@", lockPath);
      return nil;
    }
    flock(fd, LOCK_EX);

    if (sqlite3_open_v2(
            [dbPath fileSystemRepresentation], &_db, //
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL
        ) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to open DB: %s", sqlite3_errmsg(_db));
      flock(fd, LOCK_UN);
      close(fd);
      return nil;
    }

    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    NSString *sqlCreate = [NSString stringWithFormat:@"CREATE TABLE IF NOT EXISTS %@ ("
                                                      "key BLOB PRIMARY KEY, "
                                                      "value BLOB NOT NULL);",
                                                     tableName];

    char *errMsg = NULL;
    if (sqlite3_exec(_db, sqlCreate.UTF8String, NULL, NULL, &errMsg) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to create table: %s", errMsg);
      sqlite3_free(errMsg);
    }

    flock(fd, LOCK_UN);
    close(fd);

    NSString *sqlSet = [NSString stringWithFormat:@"INSERT OR REPLACE INTO %@ (key, value) VALUES (?, ?);", tableName];
    if (sqlite3_prepare_v2(_db, sqlSet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to prepare INSERT: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }
  }
  return self;
}

- (BOOL)set:(NSData *)key value:(dispatch_data_t)value {
  sqlite3_reset(_stmt);
  sqlite3_clear_bindings(_stmt);
  sqlite3_bind_blob64(_stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  const void *bytes = NULL;
  size_t length = 0;
  dispatch_data_t flat = dispatch_data_create_map(value, &bytes, &length);
  sqlite3_bind_blob64(_stmt, 2, bytes, length, SQLITE_STATIC);

  BOOL ok = sqlite3_step(_stmt) == SQLITE_DONE;
  if (!ok) {
    NSLog(@"[CacheWriter] Failed to insert: %s", sqlite3_errmsg(_db));
  }

  dispatch_release(flat);
  sqlite3_reset(_stmt);
  return ok;
}

// gw2 (#57): page_count * page_size, the main file's size (the WAL is checkpointed into it)
- (uint64_t)byteSize {
  uint64_t v[2] = {0, 0};
  const char *sql[2] = {"PRAGMA page_count;", "PRAGMA page_size;"};
  for (int i = 0; i < 2; i++) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(_db, sql[i], -1, &stmt, NULL) != SQLITE_OK)
      return 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
      v[i] = (uint64_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return v[0] * v[1];
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_db)
    sqlite3_close(_db);
  [super dealloc];
}

@end

int
_CacheReader_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheReader alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheReader_get(void *obj) {
  struct unixcall_cache_get *params = obj;
  NSData *key =
      [[NSData alloc] initWithBytesNoCopy:(void *)params->key.ptr length:params->key_length freeWhenDone:false];
  CacheReader *reader = (CacheReader *)params->cache;
  params->ret_data = (obj_handle_t)[reader get:key];
  [key release];
  return 0;
}

int
_CacheWriter_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheWriter alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheWriter_set(void *obj) {
  struct unixcall_cache_set *params = obj;
  NSData *key =
      [[NSData alloc] initWithBytesNoCopy:(void *)params->key.ptr length:params->key_length freeWhenDone:false];
  CacheWriter *writer = (CacheWriter *)params->cache;
  params->ret_success = writer && params->key_length ? [writer set:key value:(dispatch_data_t)params->value_data] : 0;
  params->ret_db_bytes = writer && params->query_size ? [writer byteSize] : 0;
  [key release];
  return 0;
}

// gw2 (W2/W3): see struct unixcall_cache_enumerate. Read-only, its own connection per call (a batch
// is hundreds of KB; the warm thread makes a handful of calls).
int
_CacheEnumerate(void *obj) {
  struct unixcall_cache_enumerate *params = obj;
  params->ret_count = 0;
  params->ret_used = 0;
  params->ret_last_rowid = params->after_rowid;
  params->ret_more = 0;
  params->ret_status = 1;
  @autoreleasepool {
    NSString *path = [NSString stringWithUTF8String:params->path.ptr];
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath || ![[NSFileManager defaultManager] fileExistsAtPath:dbPath])
      return 0;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2([dbPath fileSystemRepresentation], &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) !=
        SQLITE_OK) {
      sqlite3_close(db);
      return 0;
    }
    NSString *sql = [NSString stringWithFormat:@"SELECT rowid, key, value FROM cache_%llu WHERE rowid > ? "
                                               @"AND (? = 256 OR substr(key, 1, 1) = ?) ORDER BY rowid;",
                                               params->version];
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql.UTF8String, -1, &stmt, NULL) != SQLITE_OK) {
      sqlite3_close(db);
      return 0;
    }
    unsigned char prefix = (unsigned char)params->prefix;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)params->after_rowid);
    sqlite3_bind_int(stmt, 2, (int)params->prefix);
    sqlite3_bind_blob(stmt, 3, &prefix, 1, SQLITE_STATIC);
    params->ret_status = 0;
    unsigned char *out = params->buffer.ptr;
    uint64_t used = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const void *key = sqlite3_column_blob(stmt, 1);
      uint32_t key_len = (uint32_t)sqlite3_column_bytes(stmt, 1);
      const void *value = sqlite3_column_blob(stmt, 2);
      uint32_t value_len = (uint32_t)sqlite3_column_bytes(stmt, 2);
      uint64_t need = 8ull + key_len + value_len;
      if (used + need > params->capacity) {
        params->ret_more = 1;
        break;
      }
      for (int i = 0; i < 4; i++)
        out[used + i] = (unsigned char)(key_len >> (8 * i));
      memcpy(out + used + 4, key, key_len);
      for (int i = 0; i < 4; i++)
        out[used + 4 + key_len + i] = (unsigned char)(value_len >> (8 * i));
      memcpy(out + used + 8 + key_len, value, value_len);
      used += need;
      params->ret_count++;
      params->ret_last_rowid = (uint64_t)sqlite3_column_int64(stmt, 0);
    }
    params->ret_used = used;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
  }
  return 0;
}

#ifndef DXMT_NO_PRIVATE_API

extern void MTLSetShaderCachePath(NSString* path);
extern NSString* MTLGetShaderCachePath();

int
_WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  NSString *resolved_path = resolve_cache_dir(path, false);
  MTLSetShaderCachePath(resolved_path);
  params->ret_success = [MTLGetShaderCachePath() isEqualToString:resolved_path];
  [path release];
  return 0;
}

/* gw2 (#46): the path Metal uses (fixed at its first device creation) */
int
_WMTGetMetalShaderCachePath(void *obj) {
  struct unixcall_getmetalcachepath *params = obj;
  @autoreleasepool {
    NSString *path = MTLGetShaderCachePath();
    const char *utf8 = path ? path.UTF8String : NULL;
    size_t len = utf8 ? strlen(utf8) : 0;
    char *buf = params->buf.ptr;
    if (buf && params->size) {
      size_t n = len < params->size - 1 ? len : params->size - 1;
      if (n)
        memcpy(buf, utf8, n);
      buf[n] = 0;
    }
    params->ret_length = len;
  }
  return 0;
}

#else

int
_WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  params->ret_success = 0;
  return 0;
}

int
_WMTGetMetalShaderCachePath(void *obj) {
  struct unixcall_getmetalcachepath *params = obj;
  if (params->buf.ptr && params->size)
    ((char *)params->buf.ptr)[0] = 0;
  params->ret_length = 0;
  return 0;
}

#endif
