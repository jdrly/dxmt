#pragma once

// gw2 (#134): DXMT_CACHE_DIR, an absolute unix path that replaces $(getconf DARWIN_USER_CACHE_DIR)
// as the base of DXMT's caches (<base>/dxmt/<exe>/…, <base>/dxmt/pso/…), so each setup of the game
// on one Mac keeps its own. The unix side applies it (dxmt_cache_base in
// src/winemetal/unix/cache_dir.h); this side reads it with the same rule to log the choice once and
// to show the real base in log lines. Unset: nothing changes. Empty or relative: ignored, with one
// warning. See docs/gw2/README.md.
//
// The parsing is pure (no Windows): tests/gw2/cache_dir_test.mm checks it against the unix side.

#include <cstdint>
#include <string>

namespace dxmt::gw2 {

struct CacheDir {
  enum class State : uint8_t { Unset, Used, Ignored };
  State state = State::Unset;
  std::string value; // as set: the base (Used) or what was ignored (Ignored)
};

// What the unix side does with `raw` (nullptr: unset): used when it starts with '/'.
inline CacheDir
parseCacheDir(const char *raw) {
  if (!raw)
    return {};
  return {raw[0] == '/' ? CacheDir::State::Used : CacheDir::State::Ignored, raw};
}

// The base as a log line shows it, ending in '/' as the user cache dir does: the directory, or the
// literal "$(getconf DARWIN_USER_CACHE_DIR)" when DXMT_CACHE_DIR isn't in effect.
inline std::string
cacheBaseShown(const CacheDir &d) {
  if (d.state != CacheDir::State::Used)
    return "$(getconf DARWIN_USER_CACHE_DIR)";
  std::string s = d.value;
  while (s.size() > 1 && s.back() == '/')
    s.pop_back();
  return s == "/" ? s : s + "/";
}

// This process's DXMT_CACHE_DIR, from the environment now.
CacheDir cacheDir();

// Logs the choice once per DLL that calls it (dxgi.dll, at load): nothing when unset.
void logCacheDir();

} // namespace dxmt::gw2
