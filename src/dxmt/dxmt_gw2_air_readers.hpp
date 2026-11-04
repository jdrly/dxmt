#pragma once

// gw2 (#95, #96 review): what the startup line says about the AIR cache's read connections. Pure (no
// Metal, no Wine): tests/gw2/air_readers_test.cpp.

#include <cstdint>
#include <string>

namespace dxmt::gw2 {

// No shader cache has opened its readers yet (no device).
constexpr uint32_t kAirReadersUnopened = ~0u;

// The read connections a shader cache has open: 0 when the cache is off (DXMT_SHADER_CACHE=0) or its
// first reader couldn't be opened, else the pool's size (1 without a pool: the single reader).
inline uint32_t
airReadersOpen(bool cache_on, bool first_reader, size_t pool_size) {
  if (!cache_on || !first_reader)
    return 0;
  return pool_size ? uint32_t(pool_size) : 1;
}

// The startup line's DXMT_AIR_READERS: the connections open, or "unopened" before any device.
inline std::string
airReadersEffective(uint32_t open) {
  return open == kAirReadersUnopened ? std::string("unopened") : std::to_string(open);
}

} // namespace dxmt::gw2
