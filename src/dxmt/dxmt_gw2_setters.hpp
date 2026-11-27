#pragma once

// gw2 (#365): the setter fields of the PSO line. OMSetBlendState, OMSetDepthStencilState and RSSetState(NULL)
// mark their state for encoding on every call, also when it sets what the context already has, and each mark
// costs one command at the next draw. SOSetTargets walks its four slots on every call. These count those calls
// and the commands the draws emit, before any of them returns early (as #146 counted before
// DXMT_SKIP_SAME_STATE).
//
// gw2 (#369): under DXMT_SKIP_SAME_STATE (#146) OMSetBlendState and OMSetDepthStencilState return before they mark
// anything when they set what the context has (skipSameBlend, SameDepthStencil::skipSame). The counters count
// those calls as before.
//
// Each context counts with plain increments (PsoContextCounters, src/d3d11/d3d11_pipeline.hpp) and hands the
// counts over with its PSO counters once a chunk. With DXMT_STATS=0 nothing is counted: every method checks the
// cached `on` first, before it compares anything.

#include "dxmt_gw2_stats.hpp"
#include <cstdint>
#include <cstring>
#include <string>

namespace dxmt::gw2 {

enum class Setter : uint8_t {
  SameFactor, // samefactor: OMSetBlendState calls whose blend factor is the context's
  SameDss,    // samedss: OMSetDepthStencilState calls with the state and stencil reference the context has
  NullRs,     // nullrs: RSSetState(NULL) with no rasterizer state set
  SoSet,      // soset: SOSetTargets calls
  BlendCmd,   // blendcmd: blend factor and stencil reference commands the draws emit
  DssCmd,     // dsscmd: depth-stencil state commands the draws emit
  RsCmd,      // rscmd: rasterizer state commands the draws emit
  Count
};
constexpr unsigned kSetterCount = (unsigned)Setter::Count;

// The factor OMSetBlendState stores for `incoming` (null stores 1, 1, 1, 1) equals `current`, bit for bit: the
// command it marks would carry the same bits.
inline bool
sameBlendFactor(const float current[4], const float *incoming) {
  static constexpr float kOnes[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  return std::memcmp(current, incoming ? incoming : kOnes, sizeof(float[4])) == 0;
}

struct SetterCounters {
  bool on = statsEnabled(); // DXMT_STATS, cached: off, nothing is counted
  uint32_t n[kSetterCount] = {};
  void
  count(Setter s) {
    if (on)
      n[(unsigned)s]++;
  }
  // OMSetBlendState, before it stores the factor.
  void
  blendFactor(const float current[4], const float *incoming) {
    if (on && sameBlendFactor(current, incoming))
      n[(unsigned)Setter::SameFactor]++;
  }
  // OMSetDepthStencilState: the state and reference the context held before the call, and after it.
  void
  depthStencil(const void *before, uint32_t before_ref, const void *after, uint32_t after_ref) {
    if (on && before == after && before_ref == after_ref)
      n[(unsigned)Setter::SameDss]++;
  }
  // RSSetState(NULL), with the state it is about to clear.
  void
  nullRasterizer(const void *current) {
    if (on && !current)
      n[(unsigned)Setter::NullRs]++;
  }
  // Adds the counts to the shared totals and clears them (once a chunk).
  void publish();
  // gw2 (#369): OMSetBlendState, before it stores anything. True when `skip` (DXMT_SKIP_SAME_STATE, not DXMT_STATS)
  // is on and the call sets the blend state, factor (as sameBlendFactor compares it) and sample mask the context
  // has: the call can return before it marks the blend factor. The skipped call counts as samefactor, as the
  // call's own hook would count it.
  template <typename OutputMerger, typename BlendState>
  bool
  skipSameBlend(bool skip, const OutputMerger &om, BlendState *state, const float *factor, uint32_t mask) {
    if (!skip || state != om.BlendState || mask != om.SampleMask || !sameBlendFactor(om.BlendFactor, factor))
      return false;
    count(Setter::SameFactor);
    return true;
  }
};

// OMSetDepthStencilState's hook: made under the context's lock before the call stores anything, it keeps the
// state and reference the context holds, and when the call returns it compares them with what the call stored.
template <typename State>
class SameDepthStencil {
public:
  SameDepthStencil(SetterCounters &counters, State *const &state, const uint32_t &ref)
      : counters_(counters), state_(state), ref_(ref), before_(state), before_ref_(ref) {}
  ~SameDepthStencil() { counters_.depthStencil(before_, before_ref_, state_, ref_); }
  // gw2 (#369): true when `skip` (DXMT_SKIP_SAME_STATE) is on and the call sets the state and reference the context
  // has: the call can return before it marks the state, and this hook still counts it as samedss.
  template <typename Incoming>
  bool
  skipSame(bool skip, Incoming *state, uint32_t ref) const {
    return skip && state == state_ && ref == ref_;
  }
  SameDepthStencil(const SameDepthStencil &) = delete;
  SameDepthStencil &operator=(const SameDepthStencil &) = delete;

private:
  SetterCounters &counters_;
  State *const &state_;
  const uint32_t &ref_;
  State *before_;
  uint32_t before_ref_;
};

// Drains the shared totals into the seven fields, per frame: "samefactor=… samedss=… nullrs=… soset=…
// blendcmd=… dsscmd=… rscmd=…", zeros included. `any` says whether one of them was nonzero.
std::string setterFields(uint32_t frames, bool &any);

} // namespace dxmt::gw2
