#pragma once

// gw2 (#24): DXMT_TIMESTAMP_MODE and the TIMESTAMP section (see dxmt_gw2_stats.hpp).
//
// DXMT's timestamp query reads a command buffer's gpuEndTime (TimestampReadbackCBuf): Apple GPUs
// sample counters only at stage boundaries, and DXMT samples none. In exact mode (upstream) a
// timestamp End therefore closes the open encoder pass and commits a command buffer at the next pass
// change, so that each query gets a buffer end of its own: in GW2, about 34 command buffers and 30
// pass splits a frame. estimate and boundary leave the pass open: the encode thread marks the query
// at its position in the chunk's encoder list, and the finish thread interpolates its value between
// the GPU start and end of the command buffer it landed in (TimestampEstimateReadback).

#include <cstdint>

namespace dxmt::gw2 {

enum class TimestampMode : uint8_t {
  Exact,    // upstream: a pass split and a commit per timestamp, the buffer's gpuEndTime
  Estimate, // no pass split, no commit; the value is interpolated in the chunk's command buffers
  Boundary, // no pass split; the commit still happens at the next pass change (DXMT_PROMOTE_FLUSH_MIN applies)
};

// DXMT_TIMESTAMP_MODE, parsed and logged once per process (default estimate; an unknown value warns
// and takes the default).
TimestampMode timestampMode();
const char *timestampModeName(TimestampMode mode);

// Logs once per process, at the first query End that estimate or boundary handles.
void timestampActed();

// The render thread's counts (the immediate context), for the TIMESTAMP section. Only with DXMT_STATS.
enum class TsCount : uint8_t {
  End,      // timestamp query Ends
  Disjoint, // timestamp-disjoint query Ends
  Kept,     // Ends that found an encoder pass open and left it open (exact closes it)
  Saved,    // pass changes where exact would have committed for a timestamp and nothing committed
  Count
};
void timestampCount(TsCount what);

// The finish thread: a query of `frame` resolved to `value`; `clamped`: it was raised to the latest
// value handed out, to stay monotonic. Only with DXMT_STATS.
void timestampResolved(uint64_t frame, uint64_t value, bool clamped);

// The latest timestamp the calling thread (a finish thread) has handed out; no later query reads less.
uint64_t &timestampLatest();

} // namespace dxmt::gw2
