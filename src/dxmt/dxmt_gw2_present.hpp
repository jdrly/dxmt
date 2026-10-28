#pragma once

// gw2 (#7): when frames reach the screen, for the PRESENT section. A presented handler on each
// drawable (unix call 148, MTLDrawable_trackPresented) records the drawable's presentedTime and the
// time its present was encoded; the encode thread collects the finished records at each present.
// Off with DXMT_STATS=0, like all of the fork's measuring.

#include <cstdint>

namespace dxmt::gw2 {

// Encode thread, right before the drawable's present: adds the presented handler and collects the
// records that have finished since the last call.
void presentTrack(uint64_t drawable, uint64_t frame);

// Finish thread: a command buffer of `frame` ended on the GPU at gpu_end_ns (GPUEndTime, ns).
void presentGpuEnd(uint64_t frame, uint64_t gpu_end_ns);

// Render thread, at each Present: the refresh period and how many refreshes a frame is meant to
// stay on screen (1 = every vsync, 2 = a 30 fps cap on 60 Hz); 0 = no vsync grid (display sync
// off), so missed vsyncs aren't counted.
void presentSetPacing(double refresh_ms, uint32_t vsyncs);

} // namespace dxmt::gw2
