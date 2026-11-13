#pragma once

// gw2 (#25): Map and Unmap by resource kind and map type, logged as the MAP section (see
// dxmt_gw2_stats.hpp), and the DXMT_MAP_FAST switch.
//
// The immediate context counts every Map and Unmap in its bucket (kind x op). It times every call on
// a resource that isn't a dynamic or default-usage buffer, and a buffer call when the call's ApiScope
// times it (1 in 64 with DXMT_MAP_FAST, sampleBits), scaled as PACE scales it: a timed call costs two
// clock reads, and in Wine a clock read (QueryPerformanceCounter, see nowNs) is a syscall.
// gw2 (#150): when the ApiScope times the call, MAP takes its interval (ApiScope::stop), from before
// the device lock, instead of timing the call again; only a call the ApiScope doesn't time (a
// texture's or staging map the sampler skipped, or a nested Map) reads the clock for MAP alone, from
// after the lock. Every interval loses the clock's floor (sinceNs). Counting is plain increments under
// the device lock; publish() hands the totals over once per chunk with relaxed atomic adds, as
// FrontCounters does. With DXMT_STATS=0 nothing is counted.

#include "dxmt_gw2_stats.hpp"
#include <cstdint>

namespace dxmt::gw2 {

// DXMT_MAP_FAST (default on): the Map/Unmap fast path for dynamic and default-usage buffers.
bool mapFast();
// gw2 (#145): DXMT_UPDATE_ONE_CALL (default on, needs DXMT_MAP_FAST): the immediate context handles
// UpdateSubresource's fast branch in one call, without the nested Map and Unmap.
bool updateOneCall();

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
  uint32_t calls[kMapKindCount][kMapOpCount] = {};
  int64_t ns[kMapKindCount][kMapOpCount] = {}; // gw2 (#150): signed, as ApiTotals::ns
  uint32_t renames = 0; // a dynamic buffer took a new allocation (its page was used up)
  uint32_t created = 0; // ... and it was a new Metal buffer (none was free)
  uint32_t grown = 0;   // ... and DXMT_MAP_FAST doubled the buffer's page
  uint32_t dropped = 0; // free allocations dropped instead of reused (a smaller page, or other flags)
  // Adds the counters to the shared totals and clears them (once per chunk). Called only when `on`.
  // gw2 (#251): current_seq_id is the chunk MAP's queued.age counts from.
  void publish(uint64_t current_seq_id);
};

// gw2 (#251): the allocations queued for reuse in every live dynamic buffer and dynamic linear
// texture. With DXMT_STATS=0 no resource is listed.
struct RenameQueues {
  uint64_t pages = 0;
  uint64_t bytes = 0;
  uint64_t oldest = UINT64_MAX; // the lowest will_free_at (sequence id) of any queued allocation
  uint64_t age = 0;             // mapSeqId() after the walk minus oldest; 0 when nothing is queued
};
// The last walk's (dxmt_dynamic.cpp: a thread walks the list every 2 s); empty before the first.
RenameQueues renameQueues();
// The immediate context's current sequence id at its last chunk (MapCounters::publish).
uint64_t mapSeqId();

// One Map or Unmap: counts it in the bucket set last, timing it as the header says. Destroyed before
// its ApiScope, under the device lock.
class MapScope {
public:
  // api: the same call's ApiScope (if it times the call, this scope takes its interval)
  MapScope(MapCounters &counters, MapKind kind, MapOp op, ApiScope &api) :
      c_(counters),
      api_(api),
      kind_(kind),
      op_(op) {
    if (!c_.on)
      return;
    unsigned bits = sampleBits(Api::Map);
    if (api.scale()) {
      shared_ = true; // gw2 (#150)
      scale_ = isBufferKind(kind) ? api.scale() : 1;
      return;
    }
    if (!isBufferKind(kind))
      scale_ = 1;
    else if (!api.draw() && (api.newDraw() >> (32 - bits)) == 0) // nested: the ApiScope didn't draw
      scale_ = 1u << bits;
    if (scale_)
      t0_ = nowNs();
  }
  ~MapScope() {
    if (!c_.on)
      return;
    c_.dirty = true;
    c_.calls[(unsigned)kind_][(unsigned)op_]++;
    if (scale_)
      c_.ns[(unsigned)kind_][(unsigned)op_] += (shared_ ? api_.stop() : sinceNs(t0_)) * scale_;
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
  ApiScope &api_;
  MapKind kind_;
  MapOp op_;
  uint32_t scale_ = 0; // 0: not timed
  bool shared_ = false; // the ApiScope's interval (gw2 #150)
  int64_t t0_ = 0;
};

// gw2 (#145): one UpdateSubresource of a buffer that DXMT_UPDATE_ONE_CALL handles without its Map and
// Unmap. It counts both in MAP (`op`, then unmap) and PACE (`map`, twice), as their MapScopes and
// ApiScopes did, and PACE leaves them to an outer scope as a nested ApiScope does. One draw decides
// whether it times the update (as ApiScope samples Map: 1 in 64 since #150, scaled); both lines take
// that interval, less the clock's floor, MAP on `op`'s bucket. With stats off it does nothing.
class UpdateScope {
public:
  UpdateScope(MapCounters &counters, MapKind kind, MapOp op, ApiThreadCache &cache) :
      c_(counters),
      kind_(kind),
      op_(op) {
    if (!c_.on)
      return;
    thread_ = &cache.get();
    if ((thread_->draw() >> (32 - sampleBits(Api::Map))) == 0) {
      timed_ = true;
      t0_ = nowNs();
    }
  }
  ~UpdateScope() {
    if (!thread_)
      return;
    int64_t ns = timed_ ? sinceNs(t0_) * (1 << sampleBits(Api::Map)) : 0;
    c_.dirty = true;
    c_.calls[(unsigned)kind_][(unsigned)op_]++;
    c_.calls[(unsigned)kind_][(unsigned)MapOp::Unmap]++;
    c_.ns[(unsigned)kind_][(unsigned)op_] += ns;
    if (thread_->depth)
      return;
    thread_->call(Api::Map);
    thread_->call(Api::Map);
    if (timed_)
      thread_->add(Api::Map, ns);
  }
  UpdateScope(const UpdateScope &) = delete;
  UpdateScope &operator=(const UpdateScope &) = delete;

private:
  MapCounters &c_;
  ApiThread *thread_ = nullptr; // null: stats off
  MapKind kind_;
  MapOp op_;
  bool timed_ = false;
  int64_t t0_ = 0;
};

} // namespace dxmt::gw2
