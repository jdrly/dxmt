#pragma once

// gw2 (#7): when frames reach the screen, for the PRESENT section. A presented handler on each
// drawable (unix call 148, MTLDrawable_trackPresented) records the drawable's presentedTime and the
// time its present was encoded, and (gw2 #224) the drawable's surface; the encode thread collects
// the finished records at each present. Off with DXMT_STATS=0, like all of the fork's measuring.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace dxmt::gw2 {

// Encode thread, right before the drawable's present: adds the presented handler and collects the
// records that have finished since the last call.
void presentTrack(uint64_t drawable, uint64_t frame);

// gw2 (#324): encode thread, when the layer gave no drawable (nextDrawable returned nil: its timeout,
// or a zero drawable size). Counts it for PRESENT's nodrawable and warns once per process. True when
// DXMT_PRESENT_SKIP_NIL=1 says the present is skipped.
bool presentNoDrawable();
// gw2 (#324): DXMT_PRESENT_SKIP_NIL, =1 only, read once per process
bool presentSkipNil();

// Finish thread: a command buffer of `frame` ended on the GPU at gpu_end_ns (GPUEndTime, ns).
void presentGpuEnd(uint64_t frame, uint64_t gpu_end_ns);

// gw2 (#224), finish thread: `frame`'s present pass ran on the GPU from gpu_start_ns to gpu_end_ns
// (0: unknown), for drawwait and early. Metal 3 and DXMT_MTL4_PRESENT=mtl3: the present buffer;
// Metal 4: the pass's commit.
void presentPassGpu(uint64_t frame, uint64_t gpu_start_ns, uint64_t gpu_end_ns);

// Render thread, at each Present: the refresh period and how many refreshes a frame is meant to
// stay on screen (1 = every vsync, 2 = a 30 fps cap on 60 Hz); 0 = no vsync grid (display sync
// off), so missed vsyncs aren't counted. limiter_ms > 0: the frame limiter's interval (a cap that
// isn't a whole number of refreshes), which missed vsyncs are counted against instead of vsyncs.
void presentSetPacing(double refresh_ms, uint32_t vsyncs, double limiter_ms);

// gw2 (#314): the longest present deadline DXMT_PRESENT_DEADLINE takes. A deadline is 0 (off) or a
// finite number of ms up to this.
constexpr double kMaxPresentDeadlineMs = 1000.0;
inline bool
presentDeadlineInRange(double ms) {
  return std::isfinite(ms) && ms >= 0 && ms <= kMaxPresentDeadlineMs;
}

// gw2 (#157): the PRESENT section's skip count on a whole-refresh grid (shared with its test's oracle)
// Refreshes skipped between two shown frames `gap_ms` apart, beyond the `vsyncs` the pacing asks for.
inline int64_t
skippedRefreshes(double gap_ms, double refresh_ms, uint32_t vsyncs) {
  return std::max<int64_t>(0, std::llround(gap_ms / refresh_ms) - (int64_t)vsyncs);
}

// Whether a frame encoded at `encode_ns` was queued when the previous shown frame reached the screen
// at `last_presented_ns`: its skipped refreshes are `missed`, otherwise `starved`.
inline bool
skipWasQueued(uint64_t encode_ns, uint64_t last_presented_ns) {
  return encode_ns < last_presented_ns;
}

// gw2 (#227): DXMT_MTL4_PRESENT's grammar: exactly `mtl3` presents through a Metal 3 command buffer on a
// Metal 4 device; anything else, unset included, is the fallback, `mtl4` (today's Metal 4 present).
inline std::string
mtl4PresentGrammar(const std::string &raw, const std::string &fallback) {
  return raw == "mtl3" ? raw : fallback;
}

} // namespace dxmt::gw2
