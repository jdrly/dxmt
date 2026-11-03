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
//   the chunk's keep lists (CommandQueue::KeepUntilRetired), released in reset().
// - Pipelines, depth-stencil and sampler states: the device's caches, never evicted; DXMT's
//   internal ones (clears, blits, tile barrier, present) belong to the queue's encoding context or
//   the presenter. The present's pipelines are rebuilt only after every earlier present's chunk has
//   retired (Presenter::synchronizeLayerProperties waits on frame_presented_).
// - Fences of the pool, the queue's and the initializer's events: the queue.
// - Device teardown: with DXMT_UNRETAINED=1 or DXMT_LIFETIME_CHECK=1 the D3D11 device waits for
//   every committed chunk before its caches go, and the queue for its command buffers.
//
// DXMT_LIFETIME_CHECK=1 checks it, as d9mt's D9MT_LIFETIME_CHECK: every object a chunk uses is
// stamped with the chunk's sequence on the encode thread, and the release of DXMT's last reference
// to it before the GPU completed that chunk is logged (`LIFETIME`, once per object type). Off: one
// load and a predictable branch at each stamp site, no call.

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

// DXMT_LIFETIME_CHECK=1, read once when the DLL loads
extern const bool g_check;

inline bool
check() {
  return g_check;
}

// DXMT_UNRETAINED=1: the chunks' command buffers don't retain what they use
bool unretained();

// A command queue starts (its id for the stamps; 0 with the check off) and logs the command
// buffer mode once.
uint32_t attachQueue();

// The queue's committed command buffers are done and it goes: none of its stamps is early any more.
void detachQueue(uint32_t queue);

// Finish thread: the GPU completed chunk `seq` of `queue` (before the chunk's references go).
void completedSlow(uint32_t queue, uint64_t seq);
inline void
completed(uint32_t queue, uint64_t seq) {
  if (__builtin_expect(g_check, 0))
    completedSlow(queue, seq);
}

// Encode thread: chunk `seq` of `queue` uses `object`.
void useSlow(uint32_t queue, uint64_t seq, obj_handle_t object, Kind kind);
inline void
use(uint32_t queue, uint64_t seq, obj_handle_t object, Kind kind) {
  if (__builtin_expect(g_check, 0) && object)
    useSlow(queue, seq, object, kind);
}

// Encode thread: chunk `seq` of `queue` holds `allocation` (retainAllocation's first time per chunk).
void useAllocationSlow(uint32_t queue, uint64_t seq, Allocation *allocation);
inline void
useAllocation(uint32_t queue, uint64_t seq, Allocation *allocation) {
  if (__builtin_expect(g_check, 0))
    useAllocationSlow(queue, seq, allocation);
}

// Encode thread: every object the command list starting at `head` references (as the unix side's
// replay reads it), for an encoder of `kind`. Call only with check() on.
void useCommands(uint32_t queue, uint64_t seq, const wmtcmd_base *head, Encoder kind);

// Allocation::decRef: DXMT's last reference to `allocation` goes (before free()).
void allocationFreedSlow(Allocation *allocation);
inline void
allocationFreed(Allocation *allocation) {
  if (__builtin_expect(g_check, 0))
    allocationFreedSlow(allocation);
}

// For the startup line: "1" or "0"
const char *unretainedState();
const char *checkState();

} // namespace dxmt::gw2::lifetime
