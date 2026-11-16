#pragma once

// gw2 (#4): command-buffer counters, logged as the CMDS and ENCODE sections (see dxmt_gw2_stats.hpp).
//
// - CMDS (the game's render thread): why each chunk is committed (cbwhy), why encoder passes close
//   (close).
// - ENCODE (the encode thread): draws, dispatches, pipeline binds, useResource commands, Metal
//   encoders by type and per chunk, attachment load/store bytes by aspect, and why the encoder
//   optimizer refused to merge two render passes. Its d24bias and d24slope (#266) are counted by the
//   immediate context (FrontCounters) and handed over with CMDS's counters.
//
// Counting is plain increments on the thread that owns the counters (the immediate context, under
// the device's call order; the encode thread). Each side hands its totals over once per chunk with
// relaxed atomic adds (publish()), and the section drains them every 2 s. With DXMT_STATS=0 nothing
// is counted (each owner checks its counters' cached `on` first), published or logged.

#include "dxmt_gw2_stats.hpp"
#include <cstdint>
#include <string>

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
  Flush,   // Flush, fences, keyed mutex, WaitUntilGPUIdle
  Present,
  State,   // ExecuteCommandList, SwapDeviceContextState, FinishCommandList
  Upscale, // MetalFX through the context extension
  Copy,    // a depth-stencil copy to or from staging
  Count
};
constexpr unsigned kPassCloseCount = (unsigned)PassClose::Count;
extern const char *const kPassCloseNames[kPassCloseCount];

// gw2 (#158): the query of a GetData that would flush (without DONOTFLUSH, or a stalled poll) and found
// its data not ready. The names are the getflush= and getskip= keys (kGetDataNames).
enum class GetDataKind : uint8_t { Event, Occlusion, Timestamp, Disjoint, Count };
constexpr unsigned kGetDataKindCount = (unsigned)GetDataKind::Count;
extern const char *const kGetDataNames[kGetDataKindCount];
// Counts one such poll: `flushed` (STATS stall counts it too), or its Flush skipped by
// DXMT_GETDATA_SKIP_FLUSH. Only with DXMT_STATS; any thread (a relaxed atomic add).
void getDataPoll(GetDataKind kind, bool flushed);

// gw2 (#266): a rasterizer state's depth bias, decided when the state is created. Constant: a nonzero
// DepthBias (with or without a slope); SlopeOnly: only a SlopeScaledDepthBias.
enum class DepthBias : uint8_t { None, Constant, SlopeOnly };

constexpr DepthBias
depthBiasOf(int32_t depth_bias, float slope_scaled_depth_bias) {
  return depth_bias ? DepthBias::Constant : slope_scaled_depth_bias != 0.0f ? DepthBias::SlopeOnly : DepthBias::None;
}

// The bias the draws of a render pass get into an emulated D24 target (MTL_DXGI_FORMAT_EMULATED_D24, stored
// as Depth32Float_Stencil8): None when the pass's depth target isn't one.
constexpr DepthBias
d24PassBias(bool emulated_d24_target, DepthBias state) {
  return emulated_d24_target ? state : DepthBias::None;
}

// A D3D11_PRIMITIVE_TOPOLOGY that rasterizes triangles: triangle lists and strips, with or without adjacency,
// and patch lists (33 to 64). By the topology the game set: a geometry or domain shader's output isn't
// looked at.
constexpr bool
trianglesTopology(uint32_t topology) {
  return topology == 4 || topology == 5 || topology == 12 || topology == 13 || (topology >= 33 && topology <= 64);
}

// The RASTER line of a rasterizer state with a DepthBias or SlopeScaledDepthBias, empty for one with neither.
// fill and cull are D3D11_FILL_MODE and D3D11_CULL_MODE.
std::string rasterLine(int32_t depth_bias, float slope_scaled_depth_bias, float depth_bias_clamp, uint32_t fill,
                       uint32_t cull);

// The render thread's counters (the immediate context).
struct FrontCounters {
  bool on = statsEnabled(); // DXMT_STATS, cached: off, nothing is counted
  uint32_t commit[kCommitWhyCount] = {};
  uint32_t pass_close[kPassCloseCount] = {};
  // gw2 (#266): ENCODE's d24bias and d24slope, the triangle draws into an emulated D24 target with a
  // constant bias, and with only a slope
  uint32_t d24_bias = 0, d24_slope = 0;
  // One draw. pass_bias is the context's d24PassBias, None with DXMT_STATS=0: one branch per draw.
  void
  d24Draw(DepthBias pass_bias, uint32_t topology) {
    if (pass_bias != DepthBias::None && trianglesTopology(topology))
      (pass_bias == DepthBias::Constant ? d24_bias : d24_slope)++;
  }
  // Adds the counters to the shared totals and clears them (once per chunk).
  void publish();
};

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
  bool on = statsEnabled(); // DXMT_STATS, cached: off, nothing is counted
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
