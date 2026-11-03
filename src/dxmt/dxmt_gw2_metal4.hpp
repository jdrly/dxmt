#pragma once

// gw2 (#32): Metal 4 (DXMT_METAL4, default 0): the command queue, allocators, encoders and argument
// tables of Metal 4 instead of Metal 3's, for a device that has them (MTLGPUFamilyMetal4, macOS 26).
// The choice is made once per process, at the first command queue, and a device never mixes the
// two. The port comes in four parts (jdrly/dxmt #32): the bridge (src/winemetal, unix/mtl4.c), the
// queue and submission, the encoders and argument tables, and present. Until the last one is in,
// DXMT_METAL4=1 checks the device, logs, and stays on Metal 3. DXMT_METAL4=0 does nothing at all:
// no bridge call, no log line (the startup line shows the value).

#include "Metal.hpp"

namespace dxmt::gw2 {

// DXMT_METAL4 ("1" asks for Metal 4; anything else, or unset, is Metal 3)
bool metal4Requested();

// Whether this process's queues take the Metal 4 path (always false until #32's last part).
bool metal4Active();

// For the startup line: "0" (DXMT_METAL4 unset or 0), "1" (Metal 4 in use), "unsupported" (asked
// for, but the device or macOS has no Metal 4), "incomplete" (asked for, but this build's port
// isn't complete). Settled by the first metal4AttachQueue.
const char *metal4State();

// A command queue on `device` is being created: decide once per process and log the decision.
void metal4AttachQueue(WMT::Device device);

} // namespace dxmt::gw2
