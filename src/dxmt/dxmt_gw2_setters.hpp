#pragma once

// gw2 (#365): the setter fields of the PSO line. OMSetBlendState, OMSetDepthStencilState and RSSetState(NULL)
// mark their state for encoding on every call, also when it sets what the context already has, and each mark
// costs one command at the next draw. SOSetTargets walks its four slots on every call. These count those calls
// and the commands the draws emit, before any of them returns early (as #146 counted before
// DXMT_SKIP_SAME_STATE).
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
};

// OMSetDepthStencilState's hook: made under the context's lock before the call stores anything, it keeps the
// state and reference the context holds, and when the call returns it compares them with what the call stored.
template <typename State>
class SameDepthStencil {
public:
  SameDepthStencil(SetterCounters &counters, State *const &state, const uint32_t &ref)
      : counters_(counters), state_(state), ref_(ref), before_(state), before_ref_(ref) {}
  ~SameDepthStencil() { counters_.depthStencil(before_, before_ref_, state_, ref_); }
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
