#pragma once

// gw2 (#31): what a chunk's command buffers use stays alive until the finish thread retires the
// chunk (Metal 4 plan, phase 1; groundwork for the Metal 4 queue, #32).
//
// Metal 3's [queue commandBuffer] retains the objects DXMT sets directly (pipelines, depth-stencil
// states, attachments, bump buffers, the drawable). Metal 4's command buffers retain nothing, so
// DXMT's own bookkeeping has to hold all of it, and DXMT_UNRETAINED=1 tests that on Metal 3 with
// commandBufferWithUnretainedReferences (bridge call 154). Who holds what until the chunk retires:
// - Buffers, textures, their views and UAV counters: the chunk's AllocationRefTracking
//   (retainAllocation), released in CommandChunk::reset() on the finish thread.
// - Whatever the D3D11 commands captured: the chunk's command list, destroyed in reset().
// - Bump buffers (argument buffers, temp, staging): the ring allocators, freed by sequence after
//   the chunk retired.
// - The drawable, the events of Signal/Wait/keyed mutexes, the presenter and the MetalFX scalers:
//   the queue's retirement ledger (CommandQueue::Hold, #66), released when the chunk retires.
// - Pipelines, depth-stencil and sampler states: the device's caches, never evicted; DXMT's
//   internal ones (clears, blits, tile barrier, present) belong to the queue's encoding context or
//   the presenter. The present's pipelines are rebuilt only after every earlier present's chunk has
//   retired (Presenter::synchronizeLayerProperties waits on frame_presented_).
// - Fences of the pool, the queue's and the initializer's events: the queue.
// - Device teardown: with DXMT_UNRETAINED=1 or DXMT_LIFETIME_CHECK=1 the D3D11 device waits for
//   every committed chunk before its caches go, and the queue for its command buffers.
//
// The ledger's holds and the teardown waits act only with DXMT_UNRETAINED=1 or the check on; with both
// off, the retained command buffers hold those objects as upstream (keepUntilRetired()).
//
// DXMT_LIFETIME_CHECK=1 checks it, as d9mt's D9MT_LIFETIME_CHECK: every object a chunk uses is
// stamped with the chunk's sequence on the encode thread, and the release of DXMT's last reference
// to it before the GPU completed that chunk is logged (`LIFETIME`, once per object type). It exists
// only in a diagnostic build (meson -Dgw2_lifetime_check=true, DXMT_GW2_LIFETIME_CHECK): otherwise
// check() is a compile-time false, every stamp site and the Metal.hpp reference hook compile out,
// and DXMT_LIFETIME_CHECK=1 is ignored with a warning.

#include "winemetal.h"
#include <cstdint>

namespace dxmt {
class Allocation;
}

namespace dxmt::gw2::lifetime {

enum class Kind : uint8_t {
  Buffer,
  Texture,
  Resource, // a useResource target: a buffer or a texture
  Pipeline,
  DepthStencilState,
  SamplerState,
  Fence,
  Event,
  Drawable,
  DrawableTexture, // Core Animation holds it too: logged, but DXMT doesn't own it
  CounterSampleBuffer,
  IndirectCommandBuffer,
  Scaler,
  Count,
};

enum class Encoder : uint8_t { Render, Compute, Blit };

#ifdef DXMT_GW2_LIFETIME_CHECK
inline constexpr bool kBuilt = true;
#else
inline constexpr bool kBuilt = false;
#endif

// DXMT_LIFETIME_CHECK=1 in a diagnostic build, read once when the DLL loads; false otherwise
extern const bool g_check;

// a compile-time false without the diagnostic build
inline bool
check() {
  return kBuilt && g_check;
}

// DXMT_UNRETAINED=1 (the default; =0 is upstream's retained [queue commandBuffer]): the chunks'
// command buffers don't retain what they use
bool unretained();

// DXMT_UNRETAINED=1 or the check: DXMT itself holds what the retained command buffers would hold
// (the ledger's holds, the teardown waits). False: today's path.
bool keepUntilRetired();

// A command queue starts (its id for the stamps; 0 with the check off) and logs the command
// buffer mode once.
uint32_t attachQueue();

// The queue's committed command buffers are done and it goes: none of its stamps is early any more.
void detachQueue(uint32_t queue);

// Finish thread: the GPU completed chunk `seq` of `queue` (before the chunk's references go).
void completedSlow(uint32_t queue, uint64_t seq);
inline void
completed(uint32_t queue, uint64_t seq) {
  if (check())
    completedSlow(queue, seq);
}

// Encode thread: chunk `seq` of `queue` uses `object`.
void useSlow(uint32_t queue, uint64_t seq, obj_handle_t object, Kind kind);
inline void
use(uint32_t queue, uint64_t seq, obj_handle_t object, Kind kind) {
  if (check() && object)
    useSlow(queue, seq, object, kind);
}

// Encode thread: chunk `seq` of `queue` holds `allocation` (retainAllocation's first time per chunk).
void useAllocationSlow(uint32_t queue, uint64_t seq, Allocation *allocation);
inline void
useAllocation(uint32_t queue, uint64_t seq, Allocation *allocation) {
  if (check())
    useAllocationSlow(queue, seq, allocation);
}

// Encode thread: every object the command list starting at `head` references (as the unix side's
// replay reads it), for an encoder of `kind`. Call only with check() on.
void useCommands(uint32_t queue, uint64_t seq, const wmtcmd_base *head, Encoder kind);

// Allocation::decRef: DXMT's last reference to `allocation` goes (before free()).
void allocationFreedSlow(Allocation *allocation);
inline void
allocationFreed(Allocation *allocation) {
  if (check())
    allocationFreedSlow(allocation);
}

// For the startup line: "1" or "0"; checkState() also "unbuilt" (asked for, not in this build)
const char *unretainedState();
const char *checkState();

} // namespace dxmt::gw2::lifetime
