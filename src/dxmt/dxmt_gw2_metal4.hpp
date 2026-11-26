#pragma once

// gw2 (#32): Metal 4 (DXMT_METAL4, default 1): the command queue, allocators, encoders and argument
// tables of Metal 4 instead of Metal 3's, for a device that has them (MTLGPUFamilyMetal4, macOS 26).
// The choice is made once per process, at the first command queue, and a device never mixes the
// two. The port came in four parts (jdrly/dxmt #32): the bridge (src/winemetal, unix/mtl4.c), the
// queue and submission, the encoders and argument tables, and present. On by default since my
// in-game session of 2026-11-04 (#32's follow-up); a device without Metal 4 stays on Metal 3 with a
// warning. DXMT_METAL4=0 does nothing at all: no bridge call, no log line (the startup line shows
// the value).

#include "Metal.hpp"

namespace dxmt::gw2 {

// DXMT_METAL4 ("0" is Metal 3; anything else, or unset, asks for Metal 4: the default)
bool metal4Requested();

// gw2 (#60): DXMT_METALFX_SPATIAL_SWAPCHAIN=1 (exactly "1"), read from the environment at each call
// (as both callers always did): CreateSwapChain makes the MetalFX spatial swapchain, and the device
// stays on Metal 3.
bool metalfxSpatialSwapchain();

// gw2 (#154): DXMT_MTL4_TIMESTAMP_REUSE (off unless exactly "1"), read once per process. On, a Metal 4
// chunk's timestamp queries read its last command buffer, the present's after a present. Off (the
// default) gives them an empty command buffer of their own after it, as before #154.
bool mtl4TimestampReuse();

// gw2 (#227): DXMT_MTL4_PRESENT=mtl3 (exactly), read once per process: a queue on Metal 4 presents
// through a Metal 3 command buffer (gw2::Mtl4Queue::enableMetal3Present). A test switch for #223, off.
bool mtl4PresentMetal3();

// Whether this process's queues take the Metal 4 path (DXMT_METAL4 on, on a device that has it).
bool metal4Active();

// For the startup line: "0" (DXMT_METAL4=0), "1" (Metal 4 in use), "unsupported" (asked
// for, but the device, macOS or another setting keeps it on Metal 3). Settled by the first
// metal4AttachQueue.
const char *metal4State();

// A command queue on `device` is being created: decide once per process and log the decision.
// Metal 4 needs the residency set (DXMT_RESIDENCY_SET): Metal 4 encoders have no useResource.
void metal4AttachQueue(WMT::Device device);

// The queue's Metal 4 objects couldn't be created (gw2::Mtl4Queue::valid()): this process stays on
// Metal 3, with a warning naming `what`.
void metal4Failed(const char *what);

// A feature the Metal 4 path runs through the packet replay without having been tested there
// (geometry and tessellation emulation, deferred command lists): a warning once per kind. Issue #32
// kept them on Metal 3 "at first"; the coordinator chose (2026-11-04) to let them run on Metal 4
// instead (GW2 uses none of them, refusing them would break games).
void metal4Untested(const char *what);

} // namespace dxmt::gw2
