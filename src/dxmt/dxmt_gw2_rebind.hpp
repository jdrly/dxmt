#pragma once

// gw2 (#30): pipeline binds of the D3D11 draw path, logged as the REBIND section (see
// dxmt_gw2_stats.hpp), and the DXMT_SKIP_REBIND switch.
//
// A draw whose pipeline state changed emits a bind on the encode thread
// (ArgumentEncodingContext::bindDrawPipeline). Each render encoder tracks the pipeline bound by its
// own command list (RenderEncoderData::bound_pso): set by that bind, cleared by every other
// pipeline bind encoded into it (internal passes, the tile barrier's), and empty in a new encoder.
// A bind of the pipeline already bound counts as `same`; DXMT_SKIP_REBIND drops it from the
// command list. Counting is plain increments on the encode thread, handed over once per chunk with
// relaxed atomic adds, as ENCODE's counters are. With DXMT_STATS=0 nothing is counted (the skip
// still works).

#include "dxmt_gw2_stats.hpp"
#include <cstdint>

namespace dxmt::gw2 {

// DXMT_SKIP_REBIND (default on; =0 binds every time, as upstream). Resolved and logged once.
bool skipRebind();

// Logs once per process, the first time a bind is skipped.
void rebindActed();

// The encode thread's counters (ArgumentEncodingContext).
struct RebindCounters {
  bool on = statsEnabled(); // DXMT_STATS, cached: off, nothing is counted
  uint32_t binds = 0;       // the draw path's pipeline binds, skipped ones included
  uint32_t same = 0;        // of those, binds of the pipeline the encoder already had bound
  uint32_t skipped = 0;     // of those, the ones DXMT_SKIP_REBIND dropped
  // Adds the chunk's counters to the shared totals and clears them (end of flushCommands).
  void publishChunk();
};

} // namespace dxmt::gw2
