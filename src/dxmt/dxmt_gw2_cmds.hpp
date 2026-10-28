#pragma once

// gw2 (#4): command-buffer counters, logged as the CMDS and ENCODE sections (see dxmt_gw2_stats.hpp).
//
// - CMDS (the game's render thread): why each chunk is committed (cbwhy), why encoder passes close
//   (close), and what DXMT_PROMOTE_FLUSH_MIN did with speculative commits (promote).
// - ENCODE (the encode thread): draws, dispatches, pipeline binds, useResource commands, Metal
//   encoders by type and per chunk, attachment load/store bytes by aspect, and why the encoder
//   optimizer refused to merge two render passes.
//
// Counting is plain increments on the thread that owns the counters (the immediate context, under
// the device's call order; the encode thread). Each side hands its totals over once per chunk with
// relaxed atomic adds (publish()), and the section drains them every 2 s. With DXMT_STATS=0 nothing
// is published or logged.

#include <cstdint>

namespace dxmt::gw2 {

// Why a chunk is committed. The names are the cbwhy= keys (kCommitWhyNames).
enum class CommitWhy : uint8_t {
  None, // not set: counted as Other
  Other,
  Flush,      // the game's Flush/Flush1
  GetData,    // GetData without DONOTFLUSH on a query that isn't ready (or an event query that stalls)
  Map,        // Map of a staging resource that has to wait for the GPU
  Occlusion,  // speculative (promote_flush): occlusion query End
  Timestamp,  // speculative: timestamp or timestamp-disjoint query End
  Event,      // speculative: event query End
  Staging,    // speculative: staging -> staging copy
  Readback,   // speculative: device -> staging copy
  CmdList,    // speculative: a deferred context's command list that ended a query or copied to staging
  Present,    // Present (also an occluded one)
  Fence,      // ID3D11Fence Signal/Wait
  KeyedMutex, // AcquireSync/ReleaseSync
  Idle,       // WaitUntilGPUIdle (swapchain resize, fullscreen switch, ...)
  Poll,       // DXMT_PROMOTE_FLUSH_MIN: a deferred commit, at a GetData/Map that found its data not ready
  Timer,      // DXMT_PROMOTE_FLUSH_MIN: a deferred commit, at a pass change after DXMT_PROMOTE_FLUSH_MS
  Watchdog,   // DXMT_PROMOTE_FLUSH_MIN: a deferred commit, from the watchdog thread
  Count
};
constexpr unsigned kCommitWhyCount = (unsigned)CommitWhy::Count;
extern const char *const kCommitWhyNames[kCommitWhyCount];

// Sets the reason of the commits inside its scope, unless an outer scope already did (the outermost
// reason wins: Map -> Flush() commits as map).
class CommitWhyScope {
public:
  CommitWhyScope(CommitWhy &slot, CommitWhy why) : slot_(slot), set_(slot == CommitWhy::None) {
    if (set_)
      slot_ = why;
  }
  ~CommitWhyScope() {
    if (set_)
      slot_ = CommitWhy::None;
  }
  CommitWhyScope(const CommitWhyScope &) = delete;
  CommitWhyScope &operator=(const CommitWhyScope &) = delete;

private:
  CommitWhy &slot_;
  bool set_;
};

// Why an open encoder pass (render, compute, blit) is closed on the render thread.
enum class PassClose : uint8_t {
  Other,
  Render,  // a draw needs a render pass (after compute/blit, or a new one after a close)
  Compute, // a dispatch, or a UAV clear through compute
  Blit,    // a copy/update needs a blit pass
  Targets, // OMSetRenderTargets* changed the targets
  Clear,   // ClearRenderTargetView/DepthStencilView/View/UnorderedAccessView
  Resolve, // ResolveSubresource
  Query,   // event/timestamp query End
  Flush,   // Flush, fences, keyed mutex, WaitUntilGPUIdle, a deferred commit
  Present,
  State,   // ExecuteCommandList, SwapDeviceContextState, FinishCommandList
  Upscale, // MetalFX through the context extension
  Copy,    // a depth-stencil copy to or from staging
  Count
};
constexpr unsigned kPassCloseCount = (unsigned)PassClose::Count;
extern const char *const kPassCloseNames[kPassCloseCount];

// DXMT_PROMOTE_FLUSH_MIN: what happened at a speculative commit.
enum class Promote : uint8_t {
  Skip,     // a commit opportunity (pass change with promote_flush set) that was deferred
  Reached,  // committed: DXMT_PROMOTE_FLUSH_MIN requests had accumulated
  NoDefer,  // committed: a request that is never deferred (event, staging, readback, command list) joined
  GpuIdle,  // committed: no earlier chunk was still on the GPU
  Present,  // a deferral ended by Present's commit
  Explicit, // a deferral ended by an explicit commit (Flush, Map/GetData wait, fence, keyed mutex, idle)
  Poll,     // a deferral ended at a GetData/Map that found its data not ready
  Timer,    // a deferral ended at a pass change after DXMT_PROMOTE_FLUSH_MS
  Watchdog, // a deferral ended by the watchdog
  Count
};
constexpr unsigned kPromoteCount = (unsigned)Promote::Count;
extern const char *const kPromoteNames[kPromoteCount];

// The render thread's counters (the immediate context).
struct FrontCounters {
  uint32_t commit[kCommitWhyCount] = {};
  uint32_t pass_close[kPassCloseCount] = {};
  uint32_t promote[kPromoteCount] = {};
  // Adds the counters to the shared totals and clears them (once per chunk).
  void publish();
};

// The DXMT_PROMOTE_FLUSH_MIN in effect (0: upstream), for the CMDS line.
void setPromoteMin(uint32_t min);

// Metal encoders by type, as flushCommands encodes them (after the optimizer's merges).
enum class Enc : uint8_t { Render, Compute, Blit, Clear, Resolve, Present, Upscale, Event, Timestamp, Count };
constexpr unsigned kEncCount = (unsigned)Enc::Count;
extern const char *const kEncNames[kEncCount];

// Why the encoder optimizer didn't merge a render pass into a later one (checkEncoderRelation).
enum class Refuse : uint8_t {
  None,
  Target,      // different targets: count, depth/stencil planes or read-only flags, array length, attachment, depth plane
  ArgBuf,      // the two passes' argument buffers are in different blocks
  Load,        // the later pass doesn't load an attachment (clears it or doesn't care)
  Store,       // the earlier pass doesn't store an attachment
  VertexFence, // the later pass's vertex stage waits for the earlier pass's fragment stage
  Dependency,  // (any pair) a data dependency ends the search for a merge partner
  Sync,        // (any pair) an event or timestamp encoder ends the search
  Count
};
constexpr unsigned kRefuseCount = (unsigned)Refuse::Count;
extern const char *const kRefuseNames[kRefuseCount];

enum class Aspect : uint8_t { Color, Depth, Stencil, Count };
constexpr unsigned kAspectCount = (unsigned)Aspect::Count;

// The encode thread's counters (ArgumentEncodingContext).
struct EncodeCounters {
  uint32_t draws = 0;      // draw commands of every kind, internal ones included
  uint32_t dispatches = 0; // compute dispatches, internal ones included
  uint32_t tile = 0;       // tile dispatches (tile barriers)
  uint32_t pso = 0;        // render and compute pipeline binds
  uint32_t use_resource = 0;
  uint32_t blit_cmds = 0;
  uint32_t encoders[kEncCount] = {};
  uint32_t refuse[kRefuseCount] = {};
  uint64_t load_bytes[kAspectCount] = {};
  uint64_t store_bytes[kAspectCount] = {};
  uint32_t chunk_encoders = 0; // encoders of the chunk being flushed
  // Adds the chunk's counters to the shared totals and clears them (end of flushCommands).
  void publishChunk();
};

} // namespace dxmt::gw2
