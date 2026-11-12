#pragma once

// gw2 (#7): when frames reach the screen, for the PRESENT section. A presented handler on each
// drawable (unix call 148, MTLDrawable_trackPresented) records the drawable's presentedTime and the
// time its present was encoded; the encode thread collects the finished records at each present.
// Off with DXMT_STATS=0, like all of the fork's measuring.

#include <cstdint>
#include <string>

namespace dxmt::gw2 {

// Encode thread, right before the drawable's present: adds the presented handler and collects the
// records that have finished since the last call.
void presentTrack(uint64_t drawable, uint64_t frame);

// Finish thread: a command buffer of `frame` ended on the GPU at gpu_end_ns (GPUEndTime, ns).
void presentGpuEnd(uint64_t frame, uint64_t gpu_end_ns);

// Render thread, at each Present: the refresh period and how many refreshes a frame is meant to
// stay on screen (1 = every vsync, 2 = a 30 fps cap on 60 Hz); 0 = no vsync grid (display sync
// off), so missed vsyncs aren't counted. limiter_ms > 0: the frame limiter's interval (a cap that
// isn't a whole number of refreshes), which missed vsyncs are counted against instead of vsyncs.
void presentSetPacing(double refresh_ms, uint32_t vsyncs, double limiter_ms);

// gw2 (#157): DXMT_PRESENT_QUEUE, parsed once per process. Default: the layer keeps its three
// drawables and Present doesn't sleep. Two: the layer's maximumDrawableCount is 2. Pace: Present
// sleeps when the last nextDrawable blocked (dxmt_gw2_present_pace.hpp); it needs DXMT_STATS, whose
// presented records and nextDrawable timer it reads, and does nothing without it.
enum class PresentQueue : uint8_t { Default, Two, Pace };
PresentQueue presentQueue();
// pace asked for and DXMT_STATS on: the encode thread counts skips per frame and reports its blocks
bool presentPaceOn();

// gw2 (#227): DXMT_MTL4_PRESENT's grammar: exactly `mtl3` presents through a Metal 3 command buffer on a
// Metal 4 device; anything else, unset included, is the fallback, `mtl4` (today's Metal 4 present).
inline std::string
mtl4PresentGrammar(const std::string &raw, const std::string &fallback) {
  return raw == "mtl3" ? raw : fallback;
}

// Encode thread, with presentPaceOn(): how long the nextDrawable of present `seq` blocked.
void presentPaceBlocked(int64_t ns, uint64_t seq);

// Render thread, with presentPaceOn(): the last block and the per-frame missed and starved totals.
struct PaceSignals {
  int64_t blocked_ns = 0;
  uint64_t blocked_seq = 0;
  uint64_t missed = 0, starved = 0;
};
PaceSignals presentPaceSignals();
// Render thread: pace slept `ns` (0: it didn't), for the PRESENTQ section; gave_up once when it gave up.
void presentPaceStep(int64_t ns, bool gave_up);

} // namespace dxmt::gw2
