#include "dxmt_shader_cache.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "dxmt_gw2_switches.hpp"
#include <cstdlib>
#include <string>

namespace dxmt {

namespace {
// gw2 (#95): AIR cache readers, 1-16 (anything else, or unset: 1, today's single reader)
uint32_t
parseReaders(const std::string &raw, const uint32_t &fallback) {
  if (raw.empty())
    return fallback;
  char *end = nullptr;
  unsigned long v = std::strtoul(raw.c_str(), &end, 10);
  return end && *end == 0 && v >= 1 && v <= 16 ? uint32_t(v) : fallback;
}
sw::Switch<uint32_t> kAirReaders{"DXMT_AIR_READERS", 1, parseReaders};
std::atomic<uint32_t> g_air_readers_open{0};
// the startup line shows the readers actually open (0 before the first device)
sw::Effective kAirReadersLine{"DXMT_AIR_READERS", [] {
  auto open = g_air_readers_open.load(std::memory_order_relaxed);
  return std::to_string(open ? open : kAirReaders());
}};
} // namespace

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  static dxmt::mutex mutex;
  static std::unordered_map<WMTMetalVersion, std::unique_ptr<ShaderCache>> caches;

  std::lock_guard<dxmt::mutex> lock(mutex);
  auto iter = caches.find(version);
  if (iter == caches.end()) {
    auto inserted = caches.insert({version, std::make_unique<ShaderCache>(version)});
    return *inserted.first->second;
  }
  return *iter->second;
}

ShaderCache::ShaderCache(WMTMetalVersion metal_version) {
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0")
    return;
  std::string path;
  if (path = env::getEnvVar("DXMT_SHADER_CACHE_PATH"); !path.empty() && path.starts_with("/")) {
    if (!path.ends_with('/'))
      path += "/";
  } else {
    path = str::format("dxmt/", env::getExeName(), "/");
  }
  path += str::format("shaders_", (unsigned int)metal_version, ".db");
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
  // gw2 (#95): DXMT_AIR_READERS=n > 1: n read-only connections (each a CacheReader: its own sqlite
  // connection and statement), each behind its own lock. Fewer if a connection can't be opened.
  uint32_t n = kAirReaders();
  if (scache_reader_ && n > 1) {
    auto first = std::make_unique<ReaderSlot>();
    first->reader = scache_reader_;
    readers_.push_back(std::move(first));
    for (uint32_t i = 1; i < n; i++) {
      auto reader = WMT::CacheReader::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
      if (reader == nullptr)
        break;
      auto slot = std::make_unique<ReaderSlot>();
      slot->reader = std::move(reader);
      readers_.push_back(std::move(slot));
    }
    if (readers_.size() == 1)
      readers_.clear(); // only the one: today's path
  }
  g_air_readers_open.store(readers_.empty() ? 1 : uint32_t(readers_.size()), std::memory_order_relaxed);
}

} // namespace dxmt