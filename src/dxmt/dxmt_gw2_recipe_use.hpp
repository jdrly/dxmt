#pragma once
// gw2 (#124): the recipe file's use rows. Each session notes once per recipe that it used it (a live
// build or a warm hit), so warming can plan the recipes used most recently and most often instead of the
// oldest ones. A use row is content addressed like the file's other rows (key and value never change),
// so the insert-only recorder writes it and the warm thread's CacheEnumerate reads it:
//
//   key   'U' + the recipe's SHA-1 (its R row's key without the 'R') + the session (u64, little-endian:
//         the session's start in unix milliseconds)
//   value schema (u32, kRecipeSchema) + first use (u32: ms from the session's start to the first use)
//
// A recipe's lastSession is its newest session, sessionsUsed the number of its rows, firstUseMs the
// least first use. Pure C++: the recorder, the warm thread and the native tests share it.
#include "sha1/sha1_util.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dxmt::gw2 {

constexpr char kUseTag = 'U';
constexpr size_t kUseKeyBytes = 1 + 20 + 8;
constexpr size_t kUseValueBytes = 8;
constexpr uint32_t kUseSchema = 1; // the value's own header, as the V and R rows'

inline std::string
recipeUseKey(const Sha1Digest &recipe, uint64_t session) {
  std::string key(1, kUseTag);
  key.append(reinterpret_cast<const char *>(recipe.data), sizeof(recipe.data));
  for (int i = 0; i < 8; i++)
    key.push_back(char(uint8_t(session >> (8 * i))));
  return key;
}

inline std::vector<uint8_t>
recipeUseValue(uint32_t first_ms) {
  std::vector<uint8_t> v;
  for (uint32_t x : {kUseSchema, first_ms})
    for (int i = 0; i < 4; i++)
      v.push_back(uint8_t(x >> (8 * i)));
  return v;
}

struct RecipeUseRow {
  Sha1Digest recipe;
  uint64_t session = 0;
  uint32_t first_ms = 0;
};

// false for a row that isn't a schema-1 use row of the right size
inline bool
parseRecipeUse(const uint8_t *key, size_t key_length, const uint8_t *value, size_t value_length,
               RecipeUseRow &row) {
  if (key_length != kUseKeyBytes || value_length != kUseValueBytes || key[0] != uint8_t(kUseTag))
    return false;
  auto u32 = [](const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  };
  if (u32(value) != kUseSchema)
    return false;
  std::memcpy(row.recipe.data, key + 1, sizeof(row.recipe.data));
  row.session = 0;
  for (int i = 0; i < 8; i++)
    row.session |= uint64_t(key[21 + i]) << (8 * i);
  row.first_ms = u32(value + 4);
  return true;
}

} // namespace dxmt::gw2
