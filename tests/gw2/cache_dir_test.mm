// gw2-test: pure
// gw2-build: clang -c -x objective-c -std=gnu17 -fno-objc-arc -O1 -DDXMT_NO_PRIVATE_API -Isrc/winemetal -Isrc/winemetal/unix src/winemetal/unix/cache.c -o ${OUT}_cache.o
// gw2-build: clang++ -x objective-c++ -std=c++20 -fno-objc-arc -O1 -Wall -Wextra -Isrc/winemetal -Isrc/winemetal/unix -Isrc/dxmt tests/gw2/cache_dir_test.mm -x none ${OUT}_cache.o -framework Foundation -lsqlite3 -o $OUT
//
// gw2 (#134): native test of DXMT_CACHE_DIR, the base of DXMT's caches. No Metal.
//
// - Set to an absolute directory, a cache open through the bridge's unix side (CacheWriter and
//   CacheReader, as the AIR cache and the recipes open theirs) creates <dir>/dxmt/<exe>/<file>: the
//   dxmt/ level stays, so the layout under the base is the user cache dir's. Nothing is created in
//   the user cache dir. A trailing '/' changes nothing.
// - Unset, the base is $(getconf DARWIN_USER_CACHE_DIR), byte for byte.
// - Empty or relative, it is ignored: the user cache dir again, and no relative directory created.
// - Longer than the caller's buffer: 0 (the open fails), never a truncated path.
// - The PE side's parse (dxmt_gw2_cache_dir.hpp, which logs the choice and shows the base in log
//   lines) agrees with the unix side for each of those values.
#import <Foundation/Foundation.h>
#include <string>
#include <unistd.h>
#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "cache_dir.h"
#include "dxmt_gw2_cache_dir.hpp"
extern "C" int _CacheWriter_alloc_init(void *);
extern "C" int _CacheReader_alloc_init(void *);

using dxmt::gw2::CacheDir;

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

static std::string
userCacheDir() {
  char buf[PATH_MAX];
  return confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf)) ? std::string(buf) : std::string();
}

static std::string
base() {
  char buf[PATH_MAX];
  return dxmt_cache_base(buf, sizeof(buf)) ? std::string(buf) : std::string();
}

static bool
exists(const std::string &path) {
  return access(path.c_str(), F_OK) == 0;
}

// Opens a writer, then a reader, on `rel` (a relative cache path), as the AIR cache does.
static bool
openCache(const char *rel) {
  struct unixcall_cache_alloc_init w = {};
  w.path.ptr = rel;
  w.version = 1;
  _CacheWriter_alloc_init(&w);
  struct unixcall_cache_alloc_init r = {};
  r.path.ptr = rel;
  r.version = 1;
  _CacheReader_alloc_init(&r);
  bool ok = w.ret_cache && r.ret_cache;
  if (w.ret_cache)
    [(NSObject *)w.ret_cache release];
  if (r.ret_cache)
    [(NSObject *)r.ret_cache release];
  return ok;
}

static void
removeTree(const std::string &path) {
  [[NSFileManager defaultManager] removeItemAtPath:@(path.c_str()) error:nil];
}

// Whether the unix side and the PE side's parse agree on `raw` (nullptr: unset).
static bool
agrees(const char *raw) {
  CacheDir d = dxmt::gw2::parseCacheDir(raw);
  bool used = d.state == CacheDir::State::Used;
  return used ? base() == raw : base() == userCacheDir();
}

int
main(void) {
  @autoreleasepool {
    std::string user = userCacheDir();
    CHECK(!user.empty(), "the user cache dir is %s", user.c_str());
    char exe[64];
    snprintf(exe, sizeof(exe), "cache-dir-test-%d.exe", getpid());
    std::string rel = std::string("dxmt/") + exe + "/shaders_320.db";
    std::string in_user = user + "dxmt/" + exe;

    // unset: today's base
    unsetenv("DXMT_CACHE_DIR");
    CHECK(base() == user, "unset: the base is the user cache dir");
    CHECK(agrees(nullptr), "unset: the PE side agrees (state %d)", int(dxmt::gw2::parseCacheDir(nullptr).state));
    CHECK(dxmt::gw2::cacheBaseShown(dxmt::gw2::parseCacheDir(nullptr)) == "$(getconf DARWIN_USER_CACHE_DIR)",
          "unset: log lines show $(getconf DARWIN_USER_CACHE_DIR)");

    // absolute: <dir>/dxmt/<exe>/<file>
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "%s/gw2-cache-dir.XXXXXX", getenv("T") ?: "/tmp");
    std::string dir = mkdtemp(tmpl) ?: "";
    CHECK(!dir.empty(), "scratch base %s", dir.c_str());
    setenv("DXMT_CACHE_DIR", dir.c_str(), 1);
    CHECK(base() == dir, "absolute: the base is DXMT_CACHE_DIR");
    CHECK(agrees(dir.c_str()), "absolute: the PE side agrees");
    CHECK(openCache(rel.c_str()), "absolute: the cache opens");
    CHECK(exists(dir + "/" + rel), "absolute: created %s/%s", dir.c_str(), rel.c_str());
    CHECK(!exists(in_user), "absolute: nothing in the user cache dir (%s)", in_user.c_str());
    CHECK(dxmt::gw2::cacheBaseShown(dxmt::gw2::parseCacheDir(dir.c_str())) == dir + "/",
          "absolute: log lines show %s/", dir.c_str());

    // a trailing '/': the same file
    std::string slashed = dir + "//";
    removeTree(dir + "/dxmt");
    setenv("DXMT_CACHE_DIR", slashed.c_str(), 1);
    CHECK(openCache(rel.c_str()) && exists(dir + "/" + rel), "trailing '/': the same %s/%s", dir.c_str(),
          rel.c_str());
    CHECK(dxmt::gw2::cacheBaseShown(dxmt::gw2::parseCacheDir(slashed.c_str())) == dir + "/",
          "trailing '/': log lines show %s/", dir.c_str());
    CHECK(dxmt::gw2::cacheBaseShown(dxmt::gw2::parseCacheDir("/")) == "/", "\"/\": log lines show /");

    // too long for the caller's buffer: 0, not a truncated path
    {
      char small[8];
      CHECK(dxmt_cache_base(small, sizeof(small)) == 0, "longer than the buffer: 0");
    }

    // empty and relative: ignored, the user cache dir
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    chdir(dir.c_str());
    const char *ignored[] = {"", "rel/cache", "./cache", "~/cache"};
    for (const char *v : ignored) {
      setenv("DXMT_CACHE_DIR", v, 1);
      CacheDir d = dxmt::gw2::parseCacheDir(v);
      CHECK(base() == user, "\"%s\": the base is the user cache dir", v);
      CHECK(d.state == CacheDir::State::Ignored && agrees(v), "\"%s\": the PE side ignores it too", v);
      CHECK(dxmt::gw2::cacheBaseShown(d) == "$(getconf DARWIN_USER_CACHE_DIR)",
            "\"%s\": log lines show $(getconf DARWIN_USER_CACHE_DIR)", v);
    }
    setenv("DXMT_CACHE_DIR", "rel/cache", 1);
    CHECK(openCache(rel.c_str()) && exists(user + rel), "relative: the cache opens in the user cache dir");
    CHECK(!exists(dir + "/rel"), "relative: no directory created relative to the working directory");
    chdir(cwd);

    removeTree(in_user);
    removeTree(dir);
    unsetenv("DXMT_CACHE_DIR");
  }
  printf("%s (%d failures)\n", failures ? "FAILED" : "passed", failures);
  return failures ? 1 : 0;
}
