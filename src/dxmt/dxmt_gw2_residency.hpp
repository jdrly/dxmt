#pragma once

// gw2 (#29): one MTLResidencySet for the Metal device, attached to every DXMT command queue on it,
// instead of a useResource per resource and encoder (DXMT_RESIDENCY_SET, default on).
//
// - Buffers and textures enter the set when they are created (BufferAllocation, Texture::allocate)
//   and leave it when they are freed. An allocation is freed only when its reference count reaches
//   zero, and every chunk that used it holds a reference (ArgumentEncodingContext::access ->
//   retainAllocation) until the finish thread retires the chunk, after its last command buffer
//   completed. So no in-flight command buffer on a DXMT queue can use an allocation that leaves.
// - Left out (they keep useResource): heap-placed buffers and textures, shared (IOSurface)
//   textures, CPU-placed memory (i386 only), and everything DXMT binds as a bare Metal object
//   (the dummy constant buffer, the GS marshal's buffers). Drawables never enter it: a
//   driver-owned set pins them and starves nextDrawable (d9mt).
// - The set's adds and removes are committed by residencyBeforeCommit(), called right before every
//   command buffer commit on a DXMT queue, and only when the set changed. An allocation is created
//   (and added) before any command that uses it is encoded, so the commit before the command buffer
//   that uses it always includes it.
// - makeResident emits no useResource for an allocation whose gw2Resident flag is set (written
//   once, before the allocation is visible to another thread).
// - With DXMT_RESIDENCY_SET=0 nothing is created or flagged: makeResident is upstream's.
//
// The RESIDENCY section (2 s): resident=<allocations> mb=<their bytes> adds= removes= commits=
// devmb=<MTLDevice currentAllocatedSize>.

#include "Metal.hpp"
#include <cstdint>

namespace dxmt::gw2 {

// DXMT_RESIDENCY_SET (default on; "0" turns it off)
bool residencySetRequested();

// The set exists (on, and started); allocations may be in it.
bool residencyActive();

// "1" (the set is on), "0" (DXMT_RESIDENCY_SET=0), "failed" (asked for but not available: macOS
// before 15, or newResidencySet failed). For the startup line.
const char *residencySetState();

// A command queue on `device` was created: create the set on the first call (once per process),
// attach it to `queue` if it is on the set's device, and log the state once for this queue.
void residencyAttachQueue(WMT::Device device, WMT::CommandQueue queue);

// gw2 (#32): attaches the set to a Metal 4 queue on `device` (DXMT_METAL4), with a line. False if the
// set isn't active on that device: the caller then stays on Metal 3.
bool residencyAttachMtl4Queue(WMT::Device device, WMT::MTL4CommandQueue queue);

// Adds `count` Metal allocations (one DXMT allocation's buffer and/or texture; null handles are
// skipped) made on `device`. Returns false (nothing added) when the set is off or on another
// device; the caller then keeps useResource for them. `bytes` is for RESIDENCY's mb (counted only
// with DXMT_STATS on, like the window totals).
bool residencyAdd(WMT::Device device, const obj_handle_t *allocations, uint32_t count, uint64_t bytes);

// Removes what residencyAdd added (the same handles and bytes), when the DXMT allocation is freed.
void residencyRemove(const obj_handle_t *allocations, uint32_t count, uint64_t bytes);

// Commits the set's pending adds and removes, if any. Called right before each command buffer
// commit on a DXMT queue.
void residencyBeforeCommit();

} // namespace dxmt::gw2
