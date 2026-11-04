#pragma once

// gw2 (#114): warmRead's batches from bridge call 193, CacheEnumerate. Apart from dxmt_gw2_warm.hpp so
// the plan and its read stay testable without winemetal.h; used by the warm thread
// (d3d11_pipeline_cache.cpp) and the PSO archive tool (tools/gw2/pso_archive_tool.cpp).

#include "dxmt_gw2_recipe.hpp"
#include "dxmt_gw2_warm.hpp"
#include "winemetal.h"
#include <string>

namespace dxmt::gw2 {

// The rows of one prefix of the recipe database at `path` (as CacheReader_alloc_init's).
inline WarmEnumerate
warmEnumeratePrefix(std::string path, char prefix) {
  return [path = std::move(path), prefix](uint64_t after, uint8_t *buffer, uint64_t capacity) {
    WMTCacheEnumerateResult r{};
    CacheEnumerate(path.c_str(), kRecipeSchema, uint8_t(prefix), after, buffer, capacity, &r);
    WarmBatch b;
    b.used = r.used;
    b.last_rowid = r.last_rowid;
    b.need = r.need;
    b.need_rowid = r.need_rowid;
    b.count = r.count;
    b.status = r.status;
    b.error = r.error;
    b.busy = r.busy;
    b.more = r.more;
    return b;
  };
}

// The recipe rows ('R').
inline WarmEnumerate
warmEnumerateRecipes(std::string path) {
  return warmEnumeratePrefix(std::move(path), 'R');
}

// gw2 (#124): the use rows ('U', dxmt_gw2_recipe_use.hpp).
inline WarmEnumerate
warmEnumerateUses(std::string path) {
  return warmEnumeratePrefix(std::move(path), 'U');
}

} // namespace dxmt::gw2
