#pragma once
// gw2 (#124): the recipe file's use data, as d9mt's state cache keeps it (lastSession, sessionsUsed,
// firstUseMs per entry), so warming can plan the recipes used most recently and most often instead of
// the oldest ones. Bounded: one row per recipe ever used, rewritten in place (the bridge's CacheWriter
// is INSERT OR REPLACE), and one session counter.
//
//   'U' + the recipe's SHA-1 (its R row's key without the 'R')
//       -> schema (u32, kUseSchema), lastSession (u32), sessionsUsed (u32), firstUseMs (u32: the least
//          time from a session's start to the recipe's first use)
//   "Ssession" -> schema (u32), the last session's number (u32)
//
// Sessions are numbered by the file's own counter, not a clock: the recorder's writer reads it when it
// opens the file, and its session is that number + 1 (written back at once), so a newer session always
// has a larger number. The writer loads every U row at the same time (call 193) and merges each use of
// this session into it: once per recipe and session.
//
// Pure C++: the recorder's writer, the warm thread and the native tests share it.
#include "sha1/sha1_util.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxmt::gw2 {

constexpr char kUseTag = 'U';
constexpr size_t kUseKeyBytes = 1 + 20;
constexpr size_t kUseValueBytes = 16;
constexpr uint32_t kUseSchema = 1;
constexpr char kSessionTag = 'S';
inline const std::string kSessionKey = "Ssession";
constexpr size_t kSessionValueBytes = 8;

struct RecipeUse {
  uint32_t last_session = 0;      // the newest session that used it (the file's counter)
  uint32_t sessions = 0;          // sessions that used it
  uint32_t first_ms = UINT32_MAX; // the least first use, ms from a session's start
};

namespace use_detail {
inline uint32_t
u32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void
put(std::vector<uint8_t> &v, uint32_t x) {
  for (int i = 0; i < 4; i++)
    v.push_back(uint8_t(x >> (8 * i)));
}
} // namespace use_detail

// The recipe's SHA-1 as the use tables key it (20 bytes)
inline std::string
recipeUseId(const Sha1Digest &recipe) {
  return std::string(reinterpret_cast<const char *>(recipe.data), sizeof(recipe.data));
}

inline std::string
recipeUseKey(const std::string &id) {
  return std::string(1, kUseTag) + id;
}

inline std::vector<uint8_t>
recipeUseValue(const RecipeUse &u) {
  std::vector<uint8_t> v;
  for (uint32_t x : {kUseSchema, u.last_session, u.sessions, u.first_ms})
    use_detail::put(v, x);
  return v;
}

// A U row: false (and nothing set) unless it is a schema-1 use row of the right sizes
inline bool
parseRecipeUse(const uint8_t *key, size_t key_length, const uint8_t *value, size_t value_length, std::string &id,
               RecipeUse &use) {
  if (key_length != kUseKeyBytes || value_length != kUseValueBytes || key[0] != uint8_t(kUseTag) ||
      use_detail::u32(value) != kUseSchema)
    return false;
  id.assign(reinterpret_cast<const char *>(key) + 1, 20);
  use.last_session = use_detail::u32(value + 4);
  use.sessions = use_detail::u32(value + 8);
  use.first_ms = use_detail::u32(value + 12);
  return true;
}

inline std::vector<uint8_t>
sessionValue(uint32_t session) {
  std::vector<uint8_t> v;
  use_detail::put(v, kUseSchema);
  use_detail::put(v, session);
  return v;
}

inline bool
parseSession(const uint8_t *key, size_t key_length, const uint8_t *value, size_t value_length, uint32_t &session) {
  if (key_length != kSessionKey.size() || std::memcmp(key, kSessionKey.data(), key_length) != 0 ||
      value_length != kSessionValueBytes || use_detail::u32(value) != kUseSchema)
    return false;
  session = use_detail::u32(value + 4);
  return true;
}

// The writer's view of the use data: loaded from the file's U and S rows, then one merge per use.
struct UseLedger {
  std::unordered_map<std::string, RecipeUse> recipes; // by recipeUseId
  uint32_t stored_session = 0;                        // the counter as the file had it
  uint32_t session = 0;                               // this session (begin()); 0 before
  uint64_t malformed = 0;

  // One row of the file (any prefix): U and S rows are taken, others ignored.
  void
  load(const uint8_t *key, size_t key_length, const uint8_t *value, size_t value_length) {
    if (key_length && key[0] == uint8_t(kUseTag)) {
      std::string id;
      RecipeUse u;
      if (parseRecipeUse(key, key_length, value, value_length, id, u))
        recipes[id] = u;
      else
        malformed++;
    } else if (key_length && key[0] == uint8_t(kSessionTag)) {
      uint32_t s = 0;
      if (parseSession(key, key_length, value, value_length, s))
        stored_session = std::max(stored_session, s);
      else
        malformed++;
    }
  }

  // This session's number: one past every session the file has seen (the counter, and any U row's,
  // should a counter row have been lost). The caller writes kSessionKey -> sessionValue(session).
  uint32_t
  begin() {
    uint32_t newest = stored_session;
    for (auto &[id, u] : recipes)
      newest = std::max(newest, u.last_session);
    session = newest + 1;
    return session;
  }

  // A use in this session: the U row's new value, or false if this session already counted the recipe.
  bool
  note(const std::string &id, uint32_t first_ms, std::vector<uint8_t> &value) {
    if (!session)
      return false;
    auto &u = recipes[id];
    if (u.last_session == session)
      return false;
    u.last_session = session;
    u.sessions++;
    u.first_ms = std::min(u.first_ms, first_ms);
    value = recipeUseValue(u);
    return true;
  }
};

} // namespace dxmt::gw2
