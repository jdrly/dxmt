#pragma once

#include "Metal.hpp"
#include "d3d11_device.hpp"
#include "d3d11_input_layout.hpp"
#include "d3d11_shader.hpp"
#include "d3d11_state_object.hpp"
#include "util_hash.hpp"
#include "dxmt_gw2_stats.hpp"
#include <atomic>

struct MTL_GRAPHICS_PIPELINE_DESC {
  ManagedShader VertexShader;
  ManagedShader HullShader;
  ManagedShader DomainShader;
  ManagedShader GeometryShader;
  ManagedShader PixelShader;
  IMTLD3D11BlendState *BlendState;
  ManagedInputLayout InputLayout;
  IMTLD3D11StreamOutputLayout *SOLayout;
  UINT NumColorAttachments;
  WMTPixelFormat ColorAttachmentFormats[8];
  WMTPixelFormat DepthStencilFormat;
  WMTPrimitiveTopologyClass TopologyClass;
  bool RasterizationEnabled;
  uint8_t SampleCount;
  bool GSStripTopology;
  SM50_INDEX_BUFFER_FORMAT IndexBufferFormat;
  uint32_t SampleMask;
  uint32_t GSPassthrough;
};

namespace dxmt::gw2 {
// gw2 (#6): pipeline lookup switches (read once) and the counters of the PSO section.
struct PsoSettings {
  bool blend_hash;    // DXMT_PSO_BLEND_HASH: the blend state's fingerprint in the key's hash
  bool mru;           // DXMT_PSO_MRU: a per-context MRU in front of the pipeline cache
  bool topology_keep; // DXMT_PSO_TOPOLOGY_KEEP: keep the pipeline across topologies of one class
};
const PsoSettings &psoSettings();
// gw2 (#149): psoSettings().blend_hash for the pipeline key's hash and compare, a load instead of a
// call. psoSettings() sets it when it first reads the switches; every PipelineCache calls it in its
// constructor, before any key is hashed or compared.
extern std::atomic<bool> g_pso_blend_hash;
inline bool
psoBlendHash() {
  return g_pso_blend_hash.load(std::memory_order_relaxed);
}

// gw2 (#146): DXMT_SKIP_SAME_STATE (default on): IASetInputLayout with the bound layout, a null
// shader on an empty stage, and vertex and index buffer binds that change nothing return before
// they invalidate the pipeline or emit a command. Read once per process.
bool skipSameState();

// gw2 (#147): DXMT_SKIP_SAME_SRV (default off): XXSetShaderResources keeps a slot's hazard bit set
// when a bound slot is replaced by a view that can be an output, and then leaves a slot whose bound
// view is the incoming one before it validates the view. Read once per process.
bool skipSameSrv();

// gw2 (#146): LayoutSame, NullShaderEmpty, VertexBufferSame and IndexBufferSame count those setter
// calls (a vertex buffer per slot), with the switch on or off.
// gw2 (#147): Srv* and Sampler* count XXSetShaderResources and XXSetSamplers calls, their slots,
// the slots left as they were (SrvSame: the bound view again; not counted for samplers) and the slots
// that emitted a bind or an unbind; SrvHazardMissed counts bound slots replaced by a view that can be
// an output while the slot's hazard bit was clear. All with DXMT_SKIP_SAME_SRV on or off.
enum class PsoCounter : unsigned {
  Build,
  MruHit,
  TopologyKept,
  Lookup,
  New,
  Equal,
  LayoutSame,
  NullShaderEmpty,
  VertexBufferSame,
  IndexBufferSame,
  SrvCall,
  SrvSlot,
  SrvSame,
  SrvSet,
  SrvHazardMissed,
  SamplerCall,
  SamplerSlot,
  SamplerSet,
  Count
};
extern std::atomic<uint64_t> g_pso_counters[(unsigned)PsoCounter::Count];

// gw2 (#149): a context's PSO counters (Build, MruHit, TopologyKept, and #146's four): plain increments, added to
// g_pso_counters once a chunk (the immediate context's commit, a deferred context's
// FinishCommandList), as MapCounters does, and what is left when the context goes.
struct PsoContextCounters {
  bool on = statsEnabled(); // DXMT_STATS, cached: off, nothing is counted
  uint32_t n[(unsigned)PsoCounter::Count] = {};
  ~PsoContextCounters() { publish(); }
  void
  count(PsoCounter c) {
    if (on)
      n[(unsigned)c]++;
  }
  void
  add(PsoCounter c, uint32_t v) {
    if (on)
      n[(unsigned)c] += v;
  }
  void
  publish() {
    for (unsigned i = 0; i < (unsigned)PsoCounter::Count; i++) {
      if (n[i])
        g_pso_counters[i].fetch_add(n[i], std::memory_order_relaxed);
      n[i] = 0;
    }
  }
};
} // namespace dxmt::gw2

struct MTL_COMPUTE_PIPELINE_DESC {
  ManagedShader ComputeShader;
};

struct MTL_COMPILED_GRAPHICS_PIPELINE {
  WMT::RenderPipelineState PipelineState;
};

struct MTL_COMPILED_COMPUTE_PIPELINE {
  WMT::ComputePipelineState PipelineState;
};

struct MTL_COMPILED_TESSELLATION_MESH_PIPELINE {
  WMT::RenderPipelineState PipelineState;
  uint32_t NumControlPointOutputElement;
  uint32_t ThreadsPerPatch;
};

namespace std {
template <> struct hash<MTL_GRAPHICS_PIPELINE_DESC> {
  size_t operator()(const MTL_GRAPHICS_PIPELINE_DESC &v) const noexcept {
    dxmt::HashState state;
    state.add((size_t)v.VertexShader);
    state.add((size_t)v.PixelShader);
    state.add((size_t)v.HullShader);
    state.add((size_t)v.DomainShader);
    state.add((size_t)v.GeometryShader);
    state.add((size_t)v.InputLayout);
    /* don't add blend */
    // state.add((size_t)v.BlendState);
    // gw2 (#6): not the pointer (equal_to compares blend states by content), but the blend
    // state's fingerprint of what equal_to compares, plus RasterizationEnabled, which it also
    // compares. Without them, pipelines that differ only there share a bucket and a full hash.
    if (dxmt::gw2::psoBlendHash()) {
      state.add(v.BlendState->PipelineBlendHash(v.NumColorAttachments));
      state.add((size_t)v.RasterizationEnabled);
    }
    state.add((size_t)v.DepthStencilFormat);
    state.add((size_t)v.TopologyClass);
    state.add((size_t)v.IndexBufferFormat);
    state.add((size_t)v.GSStripTopology);
    state.add((size_t)v.SampleMask);
    state.add((size_t)v.GSPassthrough);
    state.add((size_t)v.SampleCount);
    state.add((size_t)v.NumColorAttachments);
    for (unsigned i = 0; i < v.NumColorAttachments; i++) {
      state.add(v.ColorAttachmentFormats[i]);
    }
    return state;
  };
};
template <> struct equal_to<MTL_GRAPHICS_PIPELINE_DESC> {
  bool operator()(const MTL_GRAPHICS_PIPELINE_DESC &x,
                  const MTL_GRAPHICS_PIPELINE_DESC &y) const {
    if (x.NumColorAttachments != y.NumColorAttachments)
      return false;
    for (unsigned i = 0; i < x.NumColorAttachments; i++) {
      if (x.ColorAttachmentFormats[i] != y.ColorAttachmentFormats[i])
        return false;
    }
    if (x.BlendState != y.BlendState) {
      // gw2 (#6): different fingerprints mean a field below differs; skip the two GetDesc1.
      if (dxmt::gw2::psoBlendHash() &&
          x.BlendState->PipelineBlendHash(x.NumColorAttachments) !=
              y.BlendState->PipelineBlendHash(y.NumColorAttachments))
        return false;
      D3D11_BLEND_DESC1 x_, y_;
      x.BlendState->GetDesc1(&x_);
      y.BlendState->GetDesc1(&y_);
      if (x_.IndependentBlendEnable != y_.IndependentBlendEnable)
        return false;
      if (x_.AlphaToCoverageEnable != y_.AlphaToCoverageEnable)
        return false;
      uint32_t num_blend_target =
          x_.IndependentBlendEnable ? x.NumColorAttachments : 1;
      for (unsigned i = 0; i < num_blend_target; i++) {
        auto &blend_target_x = x_.RenderTarget[i];
        auto &blend_target_y = y_.RenderTarget[i];
        if (blend_target_x.RenderTargetWriteMask !=
            blend_target_y.RenderTargetWriteMask)
          return false;
        if (blend_target_x.BlendEnable != blend_target_y.BlendEnable)
          return false;
        if (blend_target_x.LogicOpEnable != blend_target_y.LogicOpEnable)
          return false;
        if (blend_target_x.BlendEnable) {
          if (blend_target_x.BlendOp != blend_target_y.BlendOp)
            return false;
          if (blend_target_x.BlendOpAlpha != blend_target_y.BlendOpAlpha)
            return false;
          if (blend_target_x.SrcBlend != blend_target_y.SrcBlend)
            return false;
          if (blend_target_x.SrcBlendAlpha != blend_target_y.SrcBlendAlpha)
            return false;
          if (blend_target_x.DestBlend != blend_target_y.DestBlend)
            return false;
          if (blend_target_x.DestBlendAlpha != blend_target_y.DestBlendAlpha)
            return false;
        }
        if (blend_target_x.LogicOpEnable) {
          if (blend_target_x.LogicOp != blend_target_y.LogicOp)
            return false;
        }
      }
    }
    return (x.VertexShader == y.VertexShader) &&
           (x.PixelShader == y.PixelShader) && (x.HullShader == y.HullShader) &&
           (x.DomainShader == y.DomainShader) && 
           (x.GeometryShader == y.GeometryShader) &&
           (x.InputLayout == y.InputLayout) &&
           (x.DepthStencilFormat == y.DepthStencilFormat) &&
           (x.TopologyClass == y.TopologyClass) &&
           (x.RasterizationEnabled == y.RasterizationEnabled) &&
           (x.GSStripTopology == y.GSStripTopology) &&
           (x.SampleCount == y.SampleCount) &&
           (x.IndexBufferFormat == y.IndexBufferFormat) &&
           (x.SampleMask == y.SampleMask) &&
           (x.GSPassthrough == y.GSPassthrough);
  }
};
} // namespace std

namespace dxmt {

class MTLCompiledGraphicsPipeline : public ThreadpoolWork {
public:
  /**
  NOTE: the current thread is blocked if it's not ready
   */
  virtual void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pGraphicsPipeline) = 0;
  // gw2 (#88): when it is created: true if its pipeline was already done in the shared results (then
  // it is complete and isn't submitted)
  virtual bool CompleteFromShared() { return false; }
};

class MTLCompiledComputePipeline : public ThreadpoolWork {
public:
  virtual void GetPipeline(MTL_COMPILED_COMPUTE_PIPELINE *pComputePipeline) = 0;
};

class MTLCompiledGeometryPipeline : public ThreadpoolWork {
public:
  virtual void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pGeometryPipeline) = 0;
};

class MTLCompiledTessellationMeshPipeline : public ThreadpoolWork {
public:
  virtual void GetPipeline(MTL_COMPILED_TESSELLATION_MESH_PIPELINE *pTessellationPipeline) = 0;
};

std::unique_ptr<MTLCompiledGraphicsPipeline>
CreateGraphicsPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

std::unique_ptr<MTLCompiledComputePipeline> CreateComputePipeline(MTLD3D11Device *pDevice, ManagedShader ComputeShader);

std::unique_ptr<MTLCompiledGeometryPipeline>
CreateGeometryPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

std::unique_ptr<MTLCompiledTessellationMeshPipeline>
CreateTessellationMeshPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

}; // namespace dxmt