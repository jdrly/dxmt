#pragma once

// gw2: the fork's measurement plumbing (jdrly/dxmt #3). Everything here is off with DXMT_STATS=0.
//
// - Sections: a feature registers a function that returns its part of the 2 s statistics window
//   ("k=v ..."); the swapchain logs each non-empty one as its own line, "<NAME> <text>", after
//   STATS. A feature adds its numbers without touching the STATS line.
// - API time: time inside DXMT's entry points (Present, Map, Flush, Create*, Draw*, Dispatch*).
//   Draw and Dispatch each time one call in 16 of their own and scale it, Map and Unmap one in 64
//   (#150, with DXMT_MAP_FAST); all calls are counted. The sample is a per-thread pseudo-random draw
//   (sampleDraw), not a fixed position: a counter aliases with call patterns that repeat (Map/Unmap
//   alternate) and times only part of them. gw2 (#150): every timed interval loses the clock's own
//   cost (clockFloorNs), so an empty call reads 0 on average.
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
  int64_t ns[kApiCount] = {}; // sampled categories already scaled; gw2 (#150): signed, as an interval
                              // less the clock's floor can be a little below 0
};

// The calling thread's time (a presenting thread's) or the pool of the other threads'.
void apiAdd(Api api, int64_t ns);
void apiCall(Api api);

// Render-thread waits, kept with the presenting thread's interval (they overlap the API time of
// the entry point they happen in).
enum class Wait : uint32_t { Commit, Sync, Count };
constexpr unsigned kWaitCount = (unsigned)Wait::Count;
void waitAdd(Wait wait, uint64_t ns);

// gw2 (#202): PACE's `create` split by what the call made, for the CREATE line. Kept where PACE keeps
// it (the presenting thread's interval, or the other threads' pool), from the same outer scope, so the
// kinds' calls and time add up to PACE's `create`. Only with DXMT_STATS on.
enum class CreateKind : uint8_t { Buffer, Texture, View, Shader, State, Query, Count };
constexpr unsigned kCreateKindCount = (unsigned)CreateKind::Count;
extern const char *const kCreateKindNames[kCreateKindCount];
constexpr unsigned kCreateResourceKinds = 2; // Buffer and Texture: they have bytes, a size class, ...
constexpr unsigned kSizeClassCount = 4;
constexpr unsigned kUsageCount = 4; // D3D11_USAGE: DEFAULT, IMMUTABLE, DYNAMIC, STAGING
constexpr uint32_t kBindRenderTarget = 0x20, kBindDepthStencil = 0x40; // D3D11_BIND_FLAG

// under 64 KB, 64 KB to 1 MB, 1 to 16 MB, over 16 MB (KB and MB of 1024)
constexpr unsigned
sizeClass(uint64_t bytes) {
  return bytes < (64u << 10) ? 0 : bytes < (1u << 20) ? 1 : bytes <= (16u << 20) ? 2 : 3;
}

// A texture's bytes from its D3D11 description, estimated as RESIDENCY's `mb` is: every mip level
// (mips 0: the full chain), layer and sample; `unit` is a texel's bytes, or a 4x4 block's with bc.
uint64_t textureBytes(uint32_t width, uint32_t height, uint32_t depth, uint32_t layers, uint32_t mips, uint32_t unit,
                      bool bc);

// One Create*. bytes, usage, bind and init are a buffer's or texture's.
struct CreateCall {
  CreateKind kind;
  uint64_t bytes = 0;
  uint32_t usage = 0; // D3D11_USAGE
  uint32_t bind = 0;  // D3D11_BIND_FLAG
  bool init = false;  // with initial data
};

struct CreateTotals {
  uint32_t calls[kCreateKindCount] = {};
  int64_t ns[kCreateKindCount] = {}; // signed, as ApiTotals::ns
  uint64_t bytes[kCreateResourceKinds] = {};
  uint32_t init[kCreateResourceKinds] = {};
  uint32_t size[kCreateResourceKinds][kSizeClassCount] = {};
  uint32_t usage[kUsageCount] = {}; // buffers
  uint32_t rt = 0, ds = 0;          // textures bound as render target, as depth-stencil

  void add(const CreateCall &call, int64_t call_ns);
  void add(const CreateTotals &other);
  uint64_t totalCalls() const;
};

// The CREATE line's text after "CREATE ", per frame: the presenting thread's creates (PACE's `create`
// in `dxmt`), then the other threads' (`other`'s), prefixed `other.`. Empty if neither has a call.
std::string createLine(const CreateTotals &thread, const CreateTotals &other, double frames);

struct ThreadInterval {
  ApiTotals api;
  uint64_t wait_ns[kWaitCount] = {};
  uint32_t wait_count[kWaitCount] = {};
  CreateTotals create; // gw2 (#202)
};
// At the top of Present: marks the calling thread as a presenting thread and returns its API time
// and waits since its previous call, then starts over.
ThreadInterval takeThreadInterval();
// The other threads' API time since the last drain.
ApiTotals otherDrain();
// gw2 (#202): the other threads' creates since the last drain (drained with otherDrain).
CreateTotals otherCreateDrain();

// The calling thread's next pseudo-random value (xorshift32, never 0). A sampler takes its 1 in 2^bits
// from the top bits: `(draw >> (32 - bits)) == 0`.
uint32_t sampleDraw();

// gw2 (#150): a sampled ApiScope times 1 call in 2^sampleBits and scales it by that: Map and Unmap 1 in
// 64 (about 300 timed calls a frame of 19,000), the others 1 in 16
constexpr unsigned
sampleBits(Api api) {
  return api == Api::Map ? 6 : 4;
}

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
  void add(Api api, int64_t ns);
  void create(const CreateCall &call, int64_t ns); // gw2 (#202)
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
  // stats off)
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
  // gw2 (#150): how this scope scales its timed call (1, or 2^sampleBits sampled), 0 if it doesn't
  // time it
  uint32_t
  scale() const {
    return scale_;
  }
  // gw2 (#150): ends the interval of a scope that times its call (scale() != 0) and returns it,
  // unscaled, less the clock's floor. The destructor adds that one to PACE without reading the clock
  // again, so another sampler of the call (MapScope) takes the same interval.
  int64_t stop();
  // gw2 (#202): the thread of an outer scope that times its call, else null (stats off, nested, or
  // not drawn)
  ApiThread *
  timedThread() const {
    return outer_ && scale_ ? thread_ : nullptr;
  }

private:
  void begin(bool sampled, ApiThreadCache *cache);
  void end();

  ApiThread *thread_ = nullptr; // null: stats off
  Api api_;
  int64_t t0_ = 0;
  int64_t ns_ = 0;     // the interval, once stopped
  uint32_t scale_ = 0; // 0: not timed
  uint32_t draw_ = 0;
  bool outer_ = false;
  bool stopped_ = false;
};

#define GW2_API_SCOPE(cat) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, false)
#define GW2_API_SAMPLED(cat) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, true)
// gw2 (#28): the same, finding the thread through an ApiThreadCache (a context's gw2_api_thread_)
#define GW2_API_SCOPE_IN(cat, cache) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, false, &(cache))
#define GW2_API_SAMPLED_IN(cat, cache) ::dxmt::gw2::ApiScope gw2_api_scope_(::dxmt::gw2::Api::cat, true, &(cache))

// gw2 (#202): a Create* entry point's scope: PACE's `create` (an ApiScope, never sampled) and, for an
// outer scope, CREATE's kind on the same interval, so the clock is read twice a call as before. A
// nested Create* (CreateTexture2D -> CreateTexture2D1) counts in neither. With stats off it does
// nothing.
class CreateScope {
public:
  explicit CreateScope(CreateKind kind) : api_(Api::Create, false) { call_.kind = kind; }
  ~CreateScope() {
    if (auto t = api_.timedThread())
      t->create(call_, api_.stop()); // ApiScope's destructor adds the same interval to PACE
  }
  CreateScope(const CreateScope &) = delete;
  CreateScope &operator=(const CreateScope &) = delete;

  // whether this call is counted: only then describe it
  bool
  counting() const {
    return api_.timedThread();
  }
  void
  resource(uint64_t bytes, uint32_t usage, uint32_t bind, bool init) {
    call_.bytes = bytes;
    call_.usage = usage;
    call_.bind = bind;
    call_.init = init;
  }

private:
  ApiScope api_;
  CreateCall call_;
};

#define GW2_CREATE_SCOPE(kind) ::dxmt::gw2::CreateScope gw2_create_scope_(::dxmt::gw2::CreateKind::kind)

int64_t nowNs();
// gw2 (#150): the clock's own cost, what an empty interval of two nowNs() reads measures: about 52 ns
// for QueryPerformanceCounter in Wine. Measured on first use (the first timed scope), once per process:
// the median of 9 batches' mean of 256 back-to-back reads. A mean, not a median of single reads: the
// clock steps 100 ns, so a single empty interval reads 0 or 100 and only their mean is its cost.
int64_t clockFloorNs();
// gw2 (#150): a timed interval, from t0 (a nowNs() reading) to now, less clockFloorNs(). An empty one
// reads 0 on average, a single one may be a little below 0.
inline int64_t
sinceNs(int64_t t0) {
  int64_t t1 = nowNs();
  return t1 - t0 - clockFloorNs();
}

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
