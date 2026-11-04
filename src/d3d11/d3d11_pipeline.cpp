#include "Metal.hpp"
#include "d3d11_private.h"
#include "d3d11_pipeline.hpp"
#include "d3d11_device.hpp"
#include "d3d11_shader.hpp"
#include "log/log.hpp"
#include "dxmt_compile_recipe.hpp"
#include "dxmt_gw2_compile.hpp"
#include "dxmt_gw2_warm.hpp"
#include "dxmt_gw2_recipe.hpp"
#include <atomic>

namespace dxmt {

class MTLCompiledGraphicsPipelineImpl
    : public MTLCompiledGraphicsPipeline {
public:
  MTLCompiledGraphicsPipelineImpl(MTLD3D11Device *pDevice,
                              MTL_GRAPHICS_PIPELINE_DESC *pDesc)
      : num_rtvs(pDesc->NumColorAttachments),
        depth_stencil_format(pDesc->DepthStencilFormat),
        topology_class(pDesc->TopologyClass), device_(pDevice),
        shared_(pDesc->VertexShader->shared_results()), pBlendState(pDesc->BlendState),
        RasterizationEnabled(pDesc->RasterizationEnabled),
        SampleCount(pDesc->SampleCount) {
    uint32_t unorm_output_reg_mask = 0;
    for (unsigned i = 0; i < num_rtvs; i++) {
      rtv_formats[i] = pDesc->ColorAttachmentFormats[i];
      unorm_output_reg_mask |= (uint32_t(IsUnorm8RenderTargetFormat(pDesc->ColorAttachmentFormats[i])) << i);
    }

    if (pDesc->SOLayout) {
      VertexShader =
          pDesc->VertexShader->get_shader(ShaderVariantVertexStreamOutput{
              pDesc->InputLayout, (uint64_t)pDesc->SOLayout});
    } else {
      VertexShader = pDesc->VertexShader->get_shader(ShaderVariantVertex{
          pDesc->InputLayout, pDesc->GSPassthrough, !pDesc->RasterizationEnabled});
    }

    if (pDesc->PixelShader) {
      PixelShader = pDesc->PixelShader->get_shader(ShaderVariantPixel{
          pDesc->SampleMask, pDesc->BlendState->IsDualSourceBlending(),
          depth_stencil_format == WMTPixelFormatInvalid,
          unorm_output_reg_mask});
      ps_valid_render_targets = pDesc->PixelShader->reflection().PSValidRenderTargets;
    } else {
      PixelShader = nullptr;
      ps_valid_render_targets = 0;
    }
  }

  void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pPipeline) final {
    compile::demand(compile_job, ready_); // gw2 (#5, #46, C1): the encode thread waits for this compile
    *pPipeline = {state_};
  }

  // gw2 (C2): what RunThreadpoolWork probes, in its order (the declared policy waits for them)
  void GetDependencies(std::vector<ThreadpoolWork *> &deps) final {
    if (VertexShader)
      deps.push_back(VertexShader);
    if (PixelShader)
      deps.push_back(PixelShader);
  }

  // The descriptor Metal gets, except the functions (gw2 #88: also at creation, for the shared results)
  void FillInfo(WMTRenderPipelineInfo &info) {
    WMT::InitializeRenderPipelineInfo(info);

    info.rasterization_enabled = RasterizationEnabled;

    for (unsigned i = 0; i < num_rtvs; i++) {
      if (rtv_formats[i] == WMTPixelFormatInvalid)
        continue;
      info.colors[i].pixel_format = rtv_formats[i];
    }

    if (depth_stencil_format != WMTPixelFormatInvalid) {
      info.depth_pixel_format = depth_stencil_format;
    }
    if (DepthStencilPlanarFlags(depth_stencil_format) & 2) {
      info.stencil_pixel_format = depth_stencil_format;
    }

    if (pBlendState) {
      pBlendState->SetupMetalPipelineDescriptor((WMTRenderPipelineBlendInfo *)&info, num_rtvs, ps_valid_render_targets);
    }

    info.input_primitive_topology = topology_class;
    info.raster_sample_count = SampleCount;
    info.immutable_vertex_buffers = (1 << 16) | (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);
  }

  // gw2 (C4): the recipe's key in the shared results (the variants' stable keys, recorded or not)
  bool SharedKey(const compile::PipelineRecipe &recipe, Sha1Digest *key) {
    Sha1Digest vs_sha1, vs_variant, ps_sha1, ps_variant;
    bool has_ps = PixelShader && PixelShader->GetStableKey(&ps_sha1, &ps_variant);
    if (!VertexShader->GetStableKey(&vs_sha1, &vs_variant) || (PixelShader && !has_ps))
      return false;
    auto keyed = recipe;
    keyed.vs = compile::VariantKey{vs_sha1, vs_variant};
    keyed.ps.reset();
    if (has_ps)
      keyed.ps = compile::VariantKey{ps_sha1, ps_variant};
    *key = compile::recipeKey(keyed);
    return true;
  }

  bool CompleteFromShared() final {
    // gw2 (#88): a pipeline already done (warm, or another job's) is taken now
    if (!shared_)
      return false;
    WMTRenderPipelineInfo info;
    FillInfo(info);
    auto recipe = compile::recipeFromInfo(info);
    recipe.metal_version = device_->GetDXMTDevice().metalVersion();
    Sha1Digest key;
    if (!SharedKey(recipe, &key))
      return false;
    WMT::Reference<WMT::RenderPipelineState> state;
    ThreadpoolWork *owner = nullptr;
    if (shared_->pipelines.find(key, &state, &owner) != decltype(shared_->pipelines)::Claim::Done)
      return false;
    state_ = state;
    compile::completedWithoutRun(compile_job);
    gw2::warmCount(gw2::WarmCounter::LivePipelineHit);
    gw2::recipeUse(key); // gw2 (#124): a warm hit is a use too (it never reaches recipePipeline)
    SetIsDone(true);
    return true;
  }

  ThreadpoolWork *RunThreadpoolWork() {

    TRACE("Start compiling 1 PSO");

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER vs, ps;
    if (!VertexShader->GetShader(&vs)) {
      return VertexShader;
    }
    if (PixelShader && !PixelShader->GetShader(&ps)) {
      return PixelShader;
    }

    WMTRenderPipelineInfo info;
    FillInfo(info);
    info.vertex_function = vs.Function;
    if (PixelShader) {
      info.fragment_function = ps.Function;
    }
    // gw2 (C3): the pipeline's recipe, from the exact descriptor above: what is recorded (#57) and
    // what the builder builds (applied to a fresh descriptor, it gives this one back)
    auto recipe = compile::recipeFromInfo(info);
    recipe.metal_version = device_->GetDXMTDevice().metalVersion();
    if (gw2::recipeRecording()) {
      // gw2 (#57): only when every shader variant's inputs were recorded (a stream-output vertex
      // variant's never are)
      Sha1Digest vs_sha1, vs_variant, ps_sha1, ps_variant;
      bool has_ps = PixelShader && PixelShader->GetIdentity(&ps_sha1, &ps_variant);
      if (VertexShader->GetIdentity(&vs_sha1, &vs_variant) && (has_ps || !PixelShader)) {
        recipe.vs = compile::VariantKey{vs_sha1, vs_variant};
        if (has_ps)
          recipe.ps = compile::VariantKey{ps_sha1, ps_variant};
        gw2::recipePipeline(recipe);
      } else {
        gw2::recipePipelineUnsupported();
      }
    }

    // gw2 (C4): while warming is on, the shared results first: one build per recipe key
    Sha1Digest key{};
    bool keyed = false;
    if (shared_) {
      if (SharedKey(recipe, &key)) {
        keyed = true;
        WMT::Reference<WMT::RenderPipelineState> state;
        ThreadpoolWork *owner = nullptr;
        switch (shared_->pipelines.claim(key, this, &state, &owner)) {
        case decltype(shared_->pipelines)::Claim::Done:
          state_ = state;
          gw2::warmCount(gw2::WarmCounter::LivePipelineHit);
          return this;
        case decltype(shared_->pipelines)::Claim::Busy:
          gw2::warmCount(gw2::WarmCounter::LiveJoin);
          return owner; // a warm job builds it: park on it (a demand promotes it)
        case decltype(shared_->pipelines)::Claim::Mine:
          break;
        }
      }
    }

    compile::building(compile_job); // gw2 (#46, C1)
    auto t0 = gw2::compileClock(); // gw2 (#5)
    state_ = compile::Metal3PipelineBuilder(device_->GetMTLDevice())
                 .build(recipe, vs.Function, PixelShader ? ps.Function : WMT::Function{}, err); // gw2 (C3)
    gw2::compilePipelineBuilt(t0, state_ != nullptr); // gw2 (#5)
    if (keyed) {
      if (state_ != nullptr)
        shared_->pipelines.publish(key, this, state_);
      else
        shared_->pipelines.abandon(key, this);
    }

    if (state_ == nullptr) {
      ERR("Failed to create PSO: ", err.description().getUTF8String());
      return this;
    }

    TRACE("Compiled 1 PSO");

    return this;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  UINT num_rtvs;
  UINT ps_valid_render_targets;
  WMTPixelFormat rtv_formats[8];
  WMTPixelFormat depth_stencil_format;
  WMTPrimitiveTopologyClass topology_class;
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  SharedResults *shared_; // gw2 (C4): null unless warming is on
  CompiledShader *VertexShader;
  CompiledShader *PixelShader;
  IMTLD3D11BlendState *pBlendState;
  WMT::Reference<WMT::RenderPipelineState> state_;
  bool RasterizationEnabled;
  UINT SampleCount;
};

std::unique_ptr<MTLCompiledGraphicsPipeline>
CreateGraphicsPipeline(MTLD3D11Device *pDevice,
                       MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  return std::make_unique<MTLCompiledGraphicsPipelineImpl>(pDevice, pDesc);
}

class MTLCompiledComputePipelineImpl
    : public MTLCompiledComputePipeline {
public:
  MTLCompiledComputePipelineImpl(MTLD3D11Device *pDevice, ManagedShader shader)
      : device_(pDevice) {
    ComputeShader = shader->get_shader(ShaderVariantDefault{});
    uint32_t total_tgsize = shader->reflection().ThreadgroupSize[0] *
                            shader->reflection().ThreadgroupSize[1] *
                            shader->reflection().ThreadgroupSize[2];
    // FIXME: might be different on AMD GPU, if it's ever supported
    tgsize_is_multiple_of_sgwidth = (total_tgsize % 32) == 0;
  }

  void GetPipeline(MTL_COMPILED_COMPUTE_PIPELINE *pPipeline) final {
    compile::demand(compile_job, ready_); // gw2 (#5, #46, C1): the encode thread waits for this compile
    *pPipeline = {state_};
  }

  // gw2 (C2): what RunThreadpoolWork probes, in its order (the declared policy waits for them)
  void GetDependencies(std::vector<ThreadpoolWork *> &deps) final {
    if (ComputeShader)
      deps.push_back(ComputeShader);
  }

  ThreadpoolWork *RunThreadpoolWork() {

    TRACE("Start compiling 1 PSO");

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER cs;
    if (!ComputeShader->GetShader(&cs)) {
      return ComputeShader;
    }

    WMTComputePipelineInfo info;
    WMT::InitializeComputePipelineInfo(info);

    info.compute_function = cs.Function;
    gw2::recipePipelineUnsupported(); // gw2 (#57): compute recipes aren't recorded yet
    info.tgsize_is_multiple_of_sgwidth = tgsize_is_multiple_of_sgwidth;
    info.immutable_buffers = (1 << 29) | (1 << 30);

    compile::building(compile_job); // gw2 (#46, C1)
    auto t0 = gw2::compileClock(); // gw2 (#5)
    state_ = device_->GetMTLDevice().newComputePipelineState(info, err);
    gw2::compilePipelineBuilt(t0, state_ != nullptr); // gw2 (#5)

    if (!state_) {
      ERR("Failed to create compute PSO: ", err.description().getUTF8String());
      return this;
    }

    TRACE("Compiled 1 PSO");

    return this;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  CompiledShader *ComputeShader;
  WMT::Reference<WMT::ComputePipelineState> state_;
  bool tgsize_is_multiple_of_sgwidth;
};

std::unique_ptr<MTLCompiledComputePipeline>
CreateComputePipeline(MTLD3D11Device *pDevice, ManagedShader ComputeShader) {
  return std::make_unique<MTLCompiledComputePipelineImpl>(pDevice, ComputeShader);
}

} // namespace dxmt