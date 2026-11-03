#pragma once

// gw2 (#32): Metal 4 (DXMT_METAL4, default 0): the command queue, allocators, encoders and argument
// tables of Metal 4 instead of Metal 3's, for a device that has them (MTLGPUFamilyMetal4, macOS 26).
// The choice is made once per process, at the first command queue, and a device never mixes the
// two. The port came in four parts (jdrly/dxmt #32): the bridge (src/winemetal, unix/mtl4.c), the
// queue and submission, the encoders and argument tables, and present. The default stays 0 until
// the in-game A/B. DXMT_METAL4=0 does nothing at all: no bridge call, no log line (the startup line
// shows the value).

#include "Metal.hpp"

namespace dxmt::gw2 {

// DXMT_METAL4 ("1" asks for Metal 4; anything else, or unset, is Metal 3)
bool metal4Requested();

// Whether this process's queues take the Metal 4 path (DXMT_METAL4=1 on a device that has it).
bool metal4Active();

// For the startup line: "0" (DXMT_METAL4 unset or 0), "1" (Metal 4 in use), "unsupported" (asked
// for, but the device, macOS or another setting keeps it on Metal 3). Settled by the first
// metal4AttachQueue.
const char *metal4State();

// A command queue on `device` is being created: decide once per process and log the decision.
// Metal 4 needs the residency set (DXMT_RESIDENCY_SET): Metal 4 encoders have no useResource.
void metal4AttachQueue(WMT::Device device);

// The queue's Metal 4 objects couldn't be created (gw2::Mtl4Queue::valid()): this process stays on
// Metal 3, with a warning naming `what`.
void metal4Failed(const char *what);

} // namespace dxmt::gw2
