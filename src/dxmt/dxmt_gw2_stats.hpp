#pragma once

// gw2: the fork's measurement plumbing (jdrly/dxmt #3). Everything here is off with DXMT_STATS=0.
//
// - Sections: a feature registers a function that returns its part of the 2 s statistics window
//   ("k=v ..."); the swapchain logs each non-empty one as its own line, "<NAME> <text>", after
//   STATS. A feature adds its numbers without touching the STATS line.
// - API time: render-thread time inside DXMT's entry points (Present, Map, Flush, Create*,
//   Draw*, Dispatch*). Draw and Dispatch time one call in 16 and scale it; all calls are counted.
//   Nested entry points (Flush -> Flush1) count once.
// - GPU spans: the finish thread hands over each completed command buffer's GPU start/end; per
//   frame, the union of those spans and the gap to the next frame.
// - App pacing: the game's own Present-to-Present interval on its render thread.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace dxmt::gw2 {

bool statsEnabled();

using SectionFn = std::string (*)(double window_ms, uint32_t frames);
void registerSection(const char *name, SectionFn fn);
void emitSections(double window_ms, uint32_t frames);
struct SectionRegistrar {
  SectionRegistrar(const char *name, SectionFn fn) { registerSection(name, fn); }
};

enum class Api : uint32_t { Present, Map, Flush, Create, Draw, Dispatch, Count };
constexpr unsigned kApiCount = (unsigned)Api::Count;
extern const char *const kApiNames[kApiCount];

struct ApiTotals {
  uint64_t calls[kApiCount] = {};
  uint64_t ns[kApiCount] = {}; // sampled categories already scaled
};
// Totals since the last drain (all threads).
ApiTotals apiDrain();
// The calling thread's time per category since its last apiFrameReset() (for HITCH lines).
ApiTotals apiFrameOfThisThread();
void apiFrameReset();

void apiAdd(Api api, uint64_t ns);
void apiCall(Api api);

class ApiScope {
public:
  ApiScope(Api api, bool sampled);
  ~ApiScope();
  ApiScope(const ApiScope &) = delete;
  ApiScope &operator=(const ApiScope &) = delete;

private:
  Api api_;
  int64_t t0_ = 0;
  uint32_t scale_ = 0; // 0: not timed
  bool outer_ = false;
};

#define GW2_API_SCOPE(cat) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, false)
#define GW2_API_SAMPLED(cat) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, true)

int64_t nowNs();

struct GpuFrame {
  uint64_t frame;
  uint64_t span_ns;  // union of the frame's command buffers' GPU execution spans
  uint64_t first_ns; // earliest GPU start
  uint64_t last_ns;  // latest GPU end
  uint32_t buffers;
  uint32_t missing; // buffers without timestamps
};
// Finish thread, in completion order.
void gpuSpanAdd(uint64_t frame, uint64_t gpu_start_ns, uint64_t gpu_end_ns);
std::vector<GpuFrame> gpuSpanDrain();

// Render thread, at each Present: records the interval since the previous one (ms).
double appPresent();
std::vector<double> appDrain();

// A one-time line with the fork commit, every DXMT_* variable of the process, and more.
void logStartup(const std::string &extra);

} // namespace dxmt::gw2
