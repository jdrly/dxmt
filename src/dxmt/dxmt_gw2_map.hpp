#pragma once

// gw2 (#25): Map and Unmap by resource kind and map type, logged as the MAP section (see
// dxmt_gw2_stats.hpp), and the DXMT_MAP_FAST switch.
//
// The immediate context counts every Map and Unmap in its bucket (kind x op). It times every call on
// a resource that isn't a dynamic or default-usage buffer, and one buffer call in 16, scaled by 16:
// a timed call costs two clock reads, and in Wine a clock read (QueryPerformanceCounter) is a
// syscall. Counting is plain increments on the render thread; publish() hands the totals over once
// per chunk with relaxed atomic adds, as FrontCounters does. With DXMT_STATS=0 nothing is counted.

#include "dxmt_gw2_stats.hpp"
#include <cstdint>

namespace dxmt::gw2 {

// DXMT_MAP_FAST (default on): the Map/Unmap fast path for dynamic and default-usage buffers.
bool mapFast();

// What a Map/Unmap is on, from the resource (D3D11ResourceCommon::gw2_map_kind_).
enum class MapKind : uint8_t {
  Other,   // nothing Map can map (a default-usage texture, an output-bound buffer)
  Cb,      // dynamic-usage buffers, by bind flag: constant, vertex, index, anything else
  Vb,
  Ib,
  Buf,
  DefCb,   // default-usage buffers (UpdateSubresource maps them), by the same bind flags
  DefVb,
  DefIb,
  DefBuf,
  Tex,     // dynamic texture
  LinTex,  // dynamic linear texture
  Staging, // staging buffer or texture
  Count
};
constexpr unsigned kMapKindCount = (unsigned)MapKind::Count;
extern const char *const kMapKindNames[kMapKindCount];

// The bucket of a buffer from its usage (D3D11_USAGE_DEFAULT = 0, DYNAMIC = 2) and bind flags.
MapKind bufferMapKind(uint32_t usage, uint32_t bind_flags);

constexpr bool
isBufferKind(MapKind kind) {
  return kind >= MapKind::Cb && kind <= MapKind::DefBuf;
}

enum class MapOp : uint8_t { Read, Write, ReadWrite, Discard, NoOverwrite, Invalid, Unmap, Count };
constexpr unsigned kMapOpCount = (unsigned)MapOp::Count;
extern const char *const kMapOpNames[kMapOpCount];

// D3D11_MAP (READ = 1 ... WRITE_NO_OVERWRITE = 5)
constexpr MapOp
mapOp(uint32_t map_type) {
  return map_type >= 1 && map_type <= 5 ? MapOp(map_type - 1) : MapOp::Invalid;
}

// The immediate context's counters.
struct MapCounters {
  bool on = statsEnabled(); // DXMT_STATS, cached: off, nothing is counted
  bool dirty = false;
  uint32_t sample = 0;
  uint32_t calls[kMapKindCount][kMapOpCount] = {};
  uint64_t ns[kMapKindCount][kMapOpCount] = {};
  uint32_t renames = 0; // a dynamic buffer took a new allocation (its page was used up)
  uint32_t created = 0; // ... and it was a new Metal buffer (none was free)
  uint32_t grown = 0;   // ... and DXMT_MAP_FAST doubled the buffer's page
  uint32_t dropped = 0; // free allocations dropped instead of reused (a smaller page, or other flags)
  // Adds the counters to the shared totals and clears them (once per chunk).
  void publish();
};

// One Map or Unmap: counts it in the bucket set last, timing it as the header says.
class MapScope {
public:
  MapScope(MapCounters &counters, MapKind kind, MapOp op) : c_(counters), kind_(kind), op_(op) {
    if (!c_.on)
      return;
    if (!isBufferKind(kind))
      scale_ = 1;
    else if ((c_.sample++ & 15) == 8) // off the phase of ApiScope's 1-in-16, which counts from 0
      scale_ = 16;
    if (scale_)
      t0_ = nowNs();
  }
  ~MapScope() {
    if (!c_.on)
      return;
    c_.dirty = true;
    c_.calls[(unsigned)kind_][(unsigned)op_]++;
    if (scale_)
      c_.ns[(unsigned)kind_][(unsigned)op_] += uint64_t(nowNs() - t0_) * scale_;
  }
  void
  set(MapKind kind) {
    kind_ = kind;
  }
  void
  set(MapOp op) {
    op_ = op;
  }
  MapScope(const MapScope &) = delete;
  MapScope &operator=(const MapScope &) = delete;

private:
  MapCounters &c_;
  MapKind kind_;
  MapOp op_;
  uint32_t scale_ = 0; // 0: not timed
  int64_t t0_ = 0;
};

} // namespace dxmt::gw2
