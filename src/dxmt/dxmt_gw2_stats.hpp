#pragma once

// gw2: the fork's measurement plumbing (jdrly/dxmt #3). Everything here is off with DXMT_STATS=0.
//
// - Sections: a feature registers a function that returns its part of the 2 s statistics window
//   ("k=v ..."); the swapchain logs each non-empty one as its own line, "<NAME> <text>", after
//   STATS. A feature adds its numbers without touching the STATS line.
// - API time: time inside DXMT's entry points (Present, Map, Flush, Create*, Draw*, Dispatch*).
//   Draw and Dispatch each time one call in 16 of their own and scale it; all calls are counted.
//   The 1 in 16 is a per-thread pseudo-random draw (sampleDraw), not a fixed position: a counter
//   aliases with call patterns that repeat (Map/Unmap alternate) and times only part of them.
//   Nested entry points (Flush -> Flush1) count once. A thread that presents keeps its time per
//   Present-to-Present interval (takeThreadInterval); every other thread's time is pooled
//   (otherDrain), so worker threads never land in the render thread's numbers.
// - GPU spans: each command queue's finish thread hands its completed command buffers' GPU
//   start/end, split-off buffers included, to that queue's GpuCollector; per frame, the union of
//   those spans and the gap to the previous frame.

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dxmt::gw2 {

bool statsEnabled();

// DXMT_HITCH_MS, resolved (default 50; 0 turns HITCH lines off)
double hitchMs();

using SectionFn = std::string (*)(double window_ms, uint32_t frames);
void registerSection(const char *name, SectionFn fn);
void emitSections(double window_ms, uint32_t frames);
struct SectionRegistrar {
  SectionRegistrar(const char *name, SectionFn fn) { registerSection(name, fn); }
};

// PresentWait: Present's wait for frame N - maxlatency (PresentBoundary); Present is the rest of it.
enum class Api : uint32_t { Present, Map, Flush, Create, Draw, Dispatch, PresentWait, Count };
constexpr unsigned kApiCount = (unsigned)Api::Count;
extern const char *const kApiNames[kApiCount];

struct ApiTotals {
  uint64_t calls[kApiCount] = {};
  uint64_t ns[kApiCount] = {}; // sampled categories already scaled
};

// The calling thread's time (a presenting thread's) or the pool of the other threads'.
void apiAdd(Api api, uint64_t ns);
void apiCall(Api api);

// Render-thread waits, kept with the presenting thread's interval (they overlap the API time of
// the entry point they happen in).
enum class Wait : uint32_t { Commit, Sync, Count };
constexpr unsigned kWaitCount = (unsigned)Wait::Count;
void waitAdd(Wait wait, uint64_t ns);

struct ThreadInterval {
  ApiTotals api;
  uint64_t wait_ns[kWaitCount] = {};
  uint32_t wait_count[kWaitCount] = {};
};
// At the top of Present: marks the calling thread as a presenting thread and returns its API time
// and waits since its previous call, then starts over.
ThreadInterval takeThreadInterval();
// The other threads' API time since the last drain.
ApiTotals otherDrain();

// The calling thread's next pseudo-random value (xorshift32, never 0). A sampler takes its 1 in 16
// from the top 4 bits: `(draw >> 28) == k`; ApiScope uses k = 0.
uint32_t sampleDraw();

class ApiScope {
public:
  ApiScope(Api api, bool sampled);
  ~ApiScope();
  ApiScope(const ApiScope &) = delete;
  ApiScope &operator=(const ApiScope &) = delete;

  // gw2 (#25): the sampleDraw() this scope decided with, 0 if it didn't draw (not sampled, nested,
  // stats off). Another sampler of the same call can reuse it with another k, so the two never time
  // the same call.
  uint32_t
  draw() const {
    return draw_;
  }

private:
  Api api_;
  int64_t t0_ = 0;
  uint32_t scale_ = 0; // 0: not timed
  uint32_t draw_ = 0;
  bool outer_ = false;
};

#define GW2_API_SCOPE(cat) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, false)
#define GW2_API_SAMPLED(cat) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, true)

int64_t nowNs();

struct GpuFrame {
  uint64_t frame;
  uint64_t span_ns;  // union of the frame's command buffers' GPU execution spans (complete only)
  uint64_t first_ns; // earliest GPU start
  uint64_t last_ns;  // latest GPU end
  uint64_t gap_ns;   // first_ns minus the previous complete frame's last_ns (has_gap)
  uint32_t buffers;
  uint32_t missing;  // buffers without timestamps
  bool complete;     // every buffer had timestamps: span_ns and gap_ns are valid
  bool has_gap;      // the previous frame was complete too
};

// One per command queue. add() runs on that queue's finish thread only, in completion order;
// drain() hands the finished frames to the queue's consumer (its swapchain's statistics).
class GpuCollector {
public:
  void add(uint64_t frame, uint64_t gpu_start_ns, uint64_t gpu_end_ns);
  std::vector<GpuFrame> drain(uint32_t &dropped);

private:
  void finalize();

  // finish thread: the frame being collected, and the previous complete frame's end
  uint64_t frame_ = ~0ull;
  std::vector<std::pair<uint64_t, uint64_t>> spans_;
  uint32_t missing_ = 0;
  uint64_t prev_last_ns_ = 0;

  std::mutex mutex_;
  std::deque<GpuFrame> done_;
  uint32_t dropped_ = 0;
};

// True exactly once per process, for the caller that should log the startup line.
bool takeStartup();
// The startup line: the fork commit, `extra` (the effective switches and more), and every DXMT_*
// variable set in the process.
void logStartup(const std::string &extra);

} // namespace dxmt::gw2
