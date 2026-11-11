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
//   gw2 (#28): that per-thread state (nesting depth, sampler, interval) is an ApiThread, never a
//   `thread_local`: mingw-GCC's thread_local is emulated TLS (__emutls_get_address ->
//   pthread_getspecific -> TlsGetValue, GetLastError/SetLastError), about 0.1 us an access under
//   Rosetta, which cost the render thread 2.3 ms a frame at ~21,000 scopes. A context keeps the
//   last ApiThread that entered it (ApiThreadCache), checked by thread id; only a miss looks it up
//   (FlsGetValue). Fiber-local, so a thread that runs D3D11 on several fibers has one block per fiber.
//   If FlsAlloc or FlsSetValue fails, the thread keeps a block in a thread_local that is never cached
//   and always new (#121): never a reclaimed block, which an old cache may still point at.
// - GPU spans: each command queue's finish thread hands its completed command buffers' GPU
//   start/end, split-off buffers included, to that queue's GpuCollector; per frame, the union of
//   those spans and the gap to the previous frame.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dxmt::gw2 {

bool statsEnabled();
// gw2 (#60): DXMT_STATS read from the environment now (each swapchain samples it when it is made)
bool statsReadNow();

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

// gw2 (#28): one thread's API-time state; the free functions above act on the calling thread's.
// A thread takes a block on its first lookup and releases it when it exits (a fiber-local slot's
// callback); a later thread may take it again. Blocks are never freed, so a pointer stays valid.
struct ApiThread {
  std::atomic<uint32_t> tid = 0; // the owning thread's id, 0 while released; written only by its owner
                                 // (taking it) and its exit callback
  std::atomic<bool> released = false; // set by the exit callback; cleared by the next thread taking it
  bool cacheable = false; // owned through the fiber-local slot (released at exit); owner-only
  int depth = 0;             // ApiScope nesting
  uint32_t sample_state = 0; // draw()'s xorshift32 state, seeded from tid on first use
  bool presenter = false;
  ThreadInterval interval;

  uint32_t draw();
  void call(Api api);
  void add(Api api, uint64_t ns);
  void reset();
};

// The calling thread's ApiThread (one fiber-local lookup, FlsGetValue).
ApiThread &apiThread();
// At DLL_PROCESS_DETACH by FreeLibrary: frees the fiber-local slot, so its callback can't run after
// the DLL is gone.
void apiThreadsDetach();

// gw2 (#28): the last ApiThread that used an object (a device context), so its entry points find
// the calling thread's block with the thread id and a compare instead of a TLS lookup. Safe to
// share between threads: a block's tid matches only its live owner (0 once released, and a new
// owner sets its own), so a stale or racing pointer only costs a miss. Not covered: a thread killed
// by TerminateThread (no exit callback) keeps its block, and a thread that reuses its id would hit it.
class ApiThreadCache {
public:
  ApiThread &get();
  // gw2 (#149): DXMT_STATS, cached when the object is made, so its scopes don't call statsEnabled()
  bool
  stats() const {
    return stats_;
  }

private:
  bool stats_ = statsEnabled();
  std::atomic<ApiThread *> last_ = nullptr;
};

class ApiScope {
public:
  // cache: where the entry point keeps its last ApiThread and DXMT_STATS (nullptr: look the thread
  // up, call statsEnabled). gw2 (#149): the stats check is inline, the rest out of line.
  ApiScope(Api api, bool sampled, ApiThreadCache *cache = nullptr) : api_(api) {
    if (cache ? cache->stats() : statsEnabled())
      begin(sampled, cache);
  }
  ~ApiScope() {
    if (thread_)
      end();
  }
  ApiScope(const ApiScope &) = delete;
  ApiScope &operator=(const ApiScope &) = delete;

  // gw2 (#25): the sampleDraw() this scope decided with, 0 if it didn't draw (not sampled, nested,
  // stats off). Another sampler of the same call can reuse it with another k, so the two never time
  // the same call.
  uint32_t
  draw() const {
    return draw_;
  }
  // gw2 (#28): a new draw from this scope's thread, for a sampler of a call this scope didn't draw
  // for (nested); only valid with stats on
  uint32_t
  newDraw() const {
    return thread_->draw();
  }

private:
  void begin(bool sampled, ApiThreadCache *cache);
  void end();

  ApiThread *thread_ = nullptr; // null: stats off
  Api api_;
  int64_t t0_ = 0;
  uint32_t scale_ = 0; // 0: not timed
  uint32_t draw_ = 0;
  bool outer_ = false;
};

#define GW2_API_SCOPE(cat) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, false)
#define GW2_API_SAMPLED(cat) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, true)
// gw2 (#28): the same, finding the thread through an ApiThreadCache (a context's gw2_api_thread_)
#define GW2_API_SCOPE_IN(cat, cache) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, false, &(cache))
#define GW2_API_SAMPLED_IN(cat, cache) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, true, &(cache))

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
