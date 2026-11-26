#include "dxmt_gw2_cache_dir.hpp"
#include "dxmt_gw2_switches.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <atomic>
#include <cwchar>
#include <optional>
#include <windows.h>

namespace dxmt::gw2 {

namespace {

// gw2 (#134): the environment block, not getEnvVar, which can't tell an empty value from an unset
// one (an empty value is warned about) and stops at MAX_PATH.
std::optional<std::string>
rawCacheDir() {
  std::optional<std::string> out;
  if (auto block = GetEnvironmentStringsW()) {
    static constexpr wchar_t kName[] = L"DXMT_CACHE_DIR=";
    constexpr size_t kLen = sizeof(kName) / sizeof(kName[0]) - 1;
    for (const wchar_t *p = block; *p; p += wcslen(p) + 1) {
      if (!wcsncmp(p, kName, kLen)) {
        out = str::fromws(p + kLen);
        break;
      }
    }
    FreeEnvironmentStringsW(block);
  }
  return out;
}

// the startup line: the base in effect, "ignored", or "default"
sw::Effective kCacheDirLine{"DXMT_CACHE_DIR", [] {
  auto d = cacheDir();
  switch (d.state) {
  case CacheDir::State::Used: return d.value;
  case CacheDir::State::Ignored: return std::string("ignored");
  default: return std::string("default");
  }
}};

} // namespace

CacheDir
cacheDir() {
  auto raw = rawCacheDir();
  return parseCacheDir(raw ? raw->c_str() : nullptr);
}

void
logCacheDir() {
  static std::atomic<bool> done = false;
  if (done.exchange(true))
    return;
  auto d = cacheDir();
  if (d.state == CacheDir::State::Used)
    Logger::info(str::format("Caches: under ", cacheBaseShown(d),
                             "dxmt/ (DXMT_CACHE_DIR, instead of $(getconf DARWIN_USER_CACHE_DIR)dxmt/)"));
  else if (d.state == CacheDir::State::Ignored)
    Logger::warn(str::format("Caches: DXMT_CACHE_DIR=\"", d.value,
                             "\" isn't an absolute unix path; ignored, the caches stay under "
                             "$(getconf DARWIN_USER_CACHE_DIR)dxmt/"));
}

} // namespace dxmt::gw2
