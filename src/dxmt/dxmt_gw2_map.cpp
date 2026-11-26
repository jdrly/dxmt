#include "dxmt_gw2_map.hpp"
#include "dxmt_gw2_stats.hpp"
#include "util_env.hpp"
#include <atomic>
#include <format>
#include <string>

namespace dxmt::gw2 {

const char *const kMapKindNames[kMapKindCount] = {
    "other", "cb", "vb", "ib", "buf", "dcb", "dvb", "dib", "dbuf", "tex", "lintex", "staging",
};
const char *const kMapOpNames[kMapOpCount] = {
    "read", "write", "readwrite", "discard", "nooverwrite", "invalid", "unmap",
};

bool
mapFast() {
  static const bool on = env::getEnvVar("DXMT_MAP_FAST") != "0";
  return on;
}

MapKind
bufferMapKind(uint32_t usage, uint32_t bind_flags) {
  // D3D11_BIND_VERTEX_BUFFER = 1, INDEX_BUFFER = 2, CONSTANT_BUFFER = 4; D3D11_USAGE_DYNAMIC = 2
  unsigned base = usage == 2 ? (unsigned)MapKind::Cb : (unsigned)MapKind::DefCb;
  if (bind_flags & 4)
    return MapKind(base);
  if (bind_flags & 1)
    return MapKind(base + 1);
  if (bind_flags & 2)
    return MapKind(base + 2);
  return MapKind(base + 3);
}

namespace {

using counter = std::atomic<uint64_t>;

counter g_calls[kMapKindCount][kMapOpCount];
counter g_ns[kMapKindCount][kMapOpCount];
counter g_renames, g_created, g_grown, g_dropped;

inline void
add(counter &total, uint64_t value) {
  if (value)
    total.fetch_add(value, std::memory_order_relaxed);
}

inline uint64_t
take(counter &total) {
  return total.exchange(0, std::memory_order_relaxed);
}

std::string
mapSection(double, uint32_t frames) {
  if (!frames)
    return {};
  double f = frames;
  uint64_t calls[kMapKindCount][kMapOpCount], ns[kMapKindCount][kMapOpCount];
  uint64_t total_calls = 0, total_ns = 0;
  for (unsigned k = 0; k < kMapKindCount; k++) {
    for (unsigned o = 0; o < kMapOpCount; o++) {
      calls[k][o] = take(g_calls[k][o]);
      ns[k][o] = take(g_ns[k][o]);
      total_calls += calls[k][o];
      total_ns += ns[k][o];
    }
  }
  std::string out = std::format(
      "fast={} calls={:.1f} ms={:.2f} renames={:.1f} created={:.1f} grown={} dropped={}", int(mapFast()),
      total_calls / f, total_ns / 1e6 / f, take(g_renames) / f, take(g_created) / f, take(g_grown), take(g_dropped)
  );
  // per frame: calls/ms of every bucket that had a call, in kind then op order
  for (unsigned k = 0; k < kMapKindCount; k++) {
    for (unsigned o = 0; o < kMapOpCount; o++) {
      if (!calls[k][o])
        continue;
      out += std::format(
          " {}.{}={:.1f}/{:.3f}", kMapKindNames[k], kMapOpNames[o], calls[k][o] / f, ns[k][o] / 1e6 / f
      );
    }
  }
  return out;
}

SectionRegistrar s_map("MAP", &mapSection);

} // namespace

void
MapCounters::publish() {
  if (!dirty)
    return; // nothing was counted (or DXMT_STATS=0)
  for (unsigned k = 0; k < kMapKindCount; k++) {
    for (unsigned o = 0; o < kMapOpCount; o++) {
      add(g_calls[k][o], calls[k][o]);
      add(g_ns[k][o], ns[k][o]);
    }
  }
  add(g_renames, renames);
  add(g_created, created);
  add(g_grown, grown);
  add(g_dropped, dropped);
  *this = {};
}

} // namespace dxmt::gw2
