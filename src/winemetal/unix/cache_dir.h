#pragma once

/* gw2 (#134): the base of DXMT's caches, under which every relative cache path resolves (the AIR
 * cache dxmt/<exe>/shaders_*.db, the recipes dxmt/<exe>/recipes_1.db, Metal's dxmt/<exe>/com.apple.metal,
 * the PSO archive's default dxmt/pso). DXMT_CACHE_DIR when it is an absolute unix path, so a setup
 * (the GW2 on Mac app's bottle, a development bottle) can keep its caches apart; otherwise, unset,
 * empty or relative, the user cache dir ($(getconf DARWIN_USER_CACHE_DIR)) as before. The PE side
 * logs the choice once (dxmt_gw2_cache_dir.hpp, same rule). Returns what confstr returns: the length
 * with the terminating NUL, 0 on failure (here also a DXMT_CACHE_DIR longer than `size`). Read on
 * each call: no state, so the tests can link this file's users without cache.c. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline size_t
dxmt_cache_base(char *buf, size_t size) {
  const char *dir = getenv("DXMT_CACHE_DIR");
  if (dir && dir[0] == '/') {
    size_t len = strlen(dir) + 1;
    if (len > size)
      return 0;
    memcpy(buf, dir, len);
    return len;
  }
  return confstr(_CS_DARWIN_USER_CACHE_DIR, buf, size);
}
