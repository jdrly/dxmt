#include "d3d11_shader.hpp"
#include "Metal.hpp"
#include "airconv_public.h"
#include "config/config.hpp"
#include "d3d11_input_layout.hpp"
#include "dxmt_gw2_compile.hpp"
#include "dxmt_gw2_recipe.hpp"
#include "dxmt_gw2_warm.hpp"
#include "sha1/sha1_util.hpp"
#include <mutex>

namespace dxmt {

SM50_SHADER_FLAG
getGlobalShaderFlag() {
  static SM50_SHADER_FLAG shader_flag;
  static std::once_flag get_shader_flag_once;
  std::call_once(get_shader_flag_once, []() {
    shader_flag = {};
    if (Config::getInstance().getOption<bool>("d3d11.sampleNaNToZero", false))
      shader_flag |= SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO;
    if (Config::getInstance().getOption<bool>("d3d11.defuseFma", false))
      shader_flag |= SM50_SHADER_FLAG_DEFUSE_FMA;
  });
  return shader_flag;
};

template <typename Proc>
class GeneralShaderCompileTask : public CompiledShader {
public:
  GeneralShaderCompileTask(MTLD3D11Device *pDevice, ManagedShader shader,
                           Proc &&proc, std::string func_name, const Sha1Digest& variant_digest,
                           bool recorded = false)
      : CompiledShader(), proc(std::forward<Proc>(proc)), func_name(func_name), device_(pDevice),
        shader_(shader), variant_digest_(variant_digest), recorded_(recorded) {
    sm50_common.type = SM50_SHADER_COMMON;
    sm50_common.metal_version = (SM50_SHADER_METAL_VERSION)pDevice->GetDXMTDevice().metalVersion();
    sm50_common.flags = getGlobalShaderFlag();
    sm50_common.next = nullptr;
  }

  ~GeneralShaderCompileTask() {}

  bool GetShader(MTL_COMPILED_SHADER *pShaderData) final {
    bool ret = false;
    if ((ret = ready_.load(std::memory_order_acquire))) {
      *pShaderData = {function_};
    }
    return ret;
  }

  ThreadpoolWork *
  RunThreadpoolWork() {
    // gw2 (C4): while warming is on, the shared results first: one build per stable key
    auto *results = shader_->shared_results();
    if (!results)
      return Build();
    VariantId id{shader_->sha1(), variant_digest_};
    WMT::Reference<WMT::Function> function;
    ThreadpoolWork *owner = nullptr;
    switch (results->variants.claim(id, this, &function, &owner)) {
    case decltype(results->variants)::Claim::Done:
      function_ = function;
      gw2::warmCount(gw2::WarmCounter::LiveVariantHit);
      return this;
    case decltype(results->variants)::Claim::Busy:
      gw2::warmCount(gw2::WarmCounter::LiveJoin);
      return owner; // a warm job builds it: park on it (a demand promotes it)
    case decltype(results->variants)::Claim::Mine:
      break;
    }
    Build();
    if (function_ != nullptr)
      results->variants.publish(id, this, function_);
    else
      results->variants.abandon(id, this);
    return this;
  }

  ThreadpoolWork *
  Build() {
    auto pool = WMT::MakeAutoreleasePool();
    auto t0 = gw2::compileClock(); // gw2 (#5): AIR cache hit/miss and build time for COMPILE
    WMT::Reference<WMT::Error> err;
    WMT::Reference<WMT::DispatchData> lib_data = shader_->find_cached_variant(variant_digest_);

    while (lib_data != nullptr) {
      auto tl = gw2::compileClock(); // gw2 (W0)
      auto library = device_->GetMTLDevice().newLibrary(lib_data, err);
      gw2::compileVariantStage(gw2::VariantStage::Library, tl);

      if (err || !library) {
        ERR("Failed to create MTLLibrary from cache: ", err.description().getUTF8String());
        lib_data = nullptr;
        break;
      }

      auto tf = gw2::compileClock(); // gw2 (W0)
      function_ = library.newFunction(func_name.c_str());
      gw2::compileVariantStage(gw2::VariantStage::Function, tf);

      if (function_ == nullptr) {
        ERR("Failed to create MTLFunction from cache: ", func_name);
        lib_data = nullptr;
        break;
      }
      break;
    }

    gw2::compileAirLookup(lib_data != nullptr);
    if (!lib_data) {
      SM50_COMPILED_BITCODE bitcode;
      auto tx = gw2::compileClock(); // gw2 (W0)
      sm50_bitcode_t compile_result = proc(func_name.c_str(), &sm50_common);
      gw2::compileVariantStage(gw2::VariantStage::Translate, tx);

      if (!compile_result) {
        gw2::compileShaderBuilt(t0);
        return this;
      }

      SM50GetCompiledBitcode(compile_result, &bitcode);
      lib_data = WMT::MakeDispatchData(bitcode.Data, bitcode.Size);
      auto tl = gw2::compileClock(); // gw2 (W0)
      auto library = device_->GetMTLDevice().newLibrary(lib_data, err);
      gw2::compileVariantStage(gw2::VariantStage::Library, tl);

      if (err) {
        ERR("Failed to create MTLLibrary: ", err.description().getUTF8String());
        shader_->dump();
        return this;
      }

      shader_->update_cached_variant(variant_digest_, lib_data);

      SM50DestroyBitcode(compile_result);
      auto tf = gw2::compileClock(); // gw2 (W0)
      function_ = library.newFunction(func_name.c_str());
      gw2::compileVariantStage(gw2::VariantStage::Function, tf);

      if (function_ == nullptr) {
        ERR("Failed to create MTLFunction: ", func_name);
        shader_->dump();
      }
    }

    gw2::compileShaderBuilt(t0);
    return this;
  }

  bool CompleteFromShared(SharedResults *results) final {
    // gw2 (#88): a function already done (warm, or another job's) is taken now
    WMT::Reference<WMT::Function> function;
    ThreadpoolWork *owner = nullptr;
    if (results->variants.find(VariantId{shader_->sha1(), variant_digest_}, &function, &owner) !=
        decltype(results->variants)::Claim::Done)
      return false;
    function_ = function;
    compile::completedWithoutRun(compile_job);
    gw2::warmCount(gw2::WarmCounter::LiveVariantHit);
    SetIsDone(true);
    return true;
  }

  bool GetStableKey(Sha1Digest *pShader, Sha1Digest *pVariant) final {
    // gw2 (C4): the AIR cache's key, always
    *pShader = shader_->sha1();
    *pVariant = variant_digest_;
    return true;
  }

  bool GetIdentity(Sha1Digest *pShader, Sha1Digest *pVariant) final {
    // gw2 (#57): the AIR cache's key, for a variant whose inputs the recorder admitted (never the
    // stream-output, hull, domain or geometry variants, whose inputs aren't recorded yet)
    if (!recorded_)
      return false;
    *pShader = shader_->sha1();
    *pVariant = variant_digest_;
    return true;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) { ready_.store(state); }

private:
  SM50_SHADER_COMMON_DATA sm50_common;
  Proc proc;
  std::string func_name;
  MTLD3D11Device *device_;
  ManagedShader shader_;
  Sha1Digest variant_digest_;
  bool recorded_; // gw2 (#57)
  std::atomic_bool ready_;
  WMT::Reference<WMT::Function> function_;
};

template <>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *pDevice, ManagedShader shader,
                    ShaderVariantVertex variant) {
  Sha1HashState h;
  h.update(getGlobalShaderFlag());
  h.update(variant.gs_passthrough);
  h.update(variant.rasterization_disabled);
  if (variant.input_layout_handle)
    h.update(variant.input_layout_handle->sha1());
  auto variant_digest = h.final();
  bool recorded = false; // gw2 (#57): its inputs were admitted
  if (gw2::recipeRecording()) {
    // gw2 (#57): the variant's translation inputs, explicitly
    gw2::RecipeBytes in;
    in.u32(getGlobalShaderFlag());
    in.u32(variant.gs_passthrough);
    in.u8(variant.rasterization_disabled);
    in.u8(variant.input_layout_handle ? 1 : 0);
    if (variant.input_layout_handle) {
      MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC *elements = nullptr;
      uint32_t n = variant.input_layout_handle->input_layout_element(&elements);
      in.u32(variant.input_layout_handle->input_slot_mask());
      in.u32(n);
      for (uint32_t k = 0; k < n; k++) {
        in.u32(elements[k].Index);
        in.u32(elements[k].Slot);
        in.u32(elements[k].Offset);
        in.u32(elements[k].Format);
        in.u32(uint32_t(elements[k].StepFunction));
        in.u32(elements[k].InstanceStepRate);
      }
    }
    recorded = gw2::recipeVariant(shader->sha1(), variant_digest, pDevice->GetDXMTDevice().metalVersion(),
                                   gw2::RecipeVariantKind::Vertex, std::move(in));
  }
  std::string func_name = "vs_" + shader->sha1().string().substr(0, 8) + "_" + variant_digest.string();

  auto proc = [=](const char *func_name, SM50_SHADER_COMMON_DATA *common) -> sm50_bitcode_t  {
    SM50_SHADER_IA_INPUT_LAYOUT_DATA data_ia_layout;
    SM50_SHADER_GS_PASS_THROUGH_DATA data_gs_passthrough;
    data_gs_passthrough.type = SM50_SHADER_GS_PASS_THROUGH;
    data_gs_passthrough.DataEncoded = variant.gs_passthrough;
    data_gs_passthrough.RasterizationDisabled = variant.rasterization_disabled;
    data_gs_passthrough.next = common;
    if (variant.input_layout_handle) {
      data_gs_passthrough.next = &data_ia_layout;
      data_ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
      data_ia_layout.next = common;
      data_ia_layout.slot_mask = variant.input_layout_handle->input_slot_mask();
      data_ia_layout
          .num_elements = variant.input_layout_handle->input_layout_element(
          (MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC **)&data_ia_layout.elements);
    }

    sm50_bitcode_t compile_result = nullptr;
    sm50_error_t sm50_err = nullptr;
    if (SM50Compile(
            shader->handle(),
            (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data_gs_passthrough,
            func_name, &compile_result, &sm50_err)) {
      ERR("Failed to compile shader: ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      return nullptr;
    }
    return compile_result;
  };
  return std::make_unique<GeneralShaderCompileTask<decltype(proc)>>(
      pDevice, shader, std::move(proc), func_name, variant_digest, recorded);
};

template <>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *pDevice, ManagedShader shader,
                    ShaderVariantPixel variant) {
  Sha1HashState h;
  h.update(getGlobalShaderFlag());
  h.update(variant.sample_mask);
  h.update(variant.unorm_output_reg_mask);
  h.update(variant.dual_source_blending);
  h.update(variant.disable_depth_output);
  auto variant_digest = h.final();
  bool recorded = false; // gw2 (#57): its inputs were admitted
  if (gw2::recipeRecording()) {
    // gw2 (#57): the variant's translation inputs, explicitly
    gw2::RecipeBytes in;
    in.u32(getGlobalShaderFlag());
    in.u32(variant.sample_mask);
    in.u32(variant.unorm_output_reg_mask);
    in.u8(variant.dual_source_blending);
    in.u8(variant.disable_depth_output);
    recorded = gw2::recipeVariant(shader->sha1(), variant_digest, pDevice->GetDXMTDevice().metalVersion(),
                                   gw2::RecipeVariantKind::Pixel, std::move(in));
  }
  std::string func_name = "ps_" + shader->sha1().string().substr(0, 8) + "_" + variant_digest.string();

  auto proc = [=](const char *func_name, SM50_SHADER_COMMON_DATA *common) -> sm50_bitcode_t  {
    SM50_SHADER_PSO_PIXEL_SHADER_DATA data;
    data.type = SM50_SHADER_PSO_PIXEL_SHADER;
    data.next = common;
    data.sample_mask = variant.sample_mask;
    data.dual_source_blending = variant.dual_source_blending;
    data.disable_depth_output = variant.disable_depth_output;
    data.unorm_output_reg_mask = variant.unorm_output_reg_mask;
    memset(data.pixel_formats, 0, sizeof(data.pixel_formats));

    sm50_bitcode_t compile_result = nullptr;
    sm50_error_t sm50_err = nullptr;
    if (SM50Compile(shader->handle(),
                               (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data,
                               func_name, &compile_result, &sm50_err)) {
      ERR("Failed to compile shader: ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      return nullptr;
    }
    return compile_result;
  };
  return std::make_unique<GeneralShaderCompileTask<decltype(proc)>>(
      pDevice, shader, std::move(proc), func_name, variant_digest, recorded);
};

template <>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *pDevice, ManagedShader shader,
                    ShaderVariantDefault) {
  Sha1HashState h;
  h.update(getGlobalShaderFlag());
  auto variant_digest = h.final();
  bool recorded = false; // gw2 (#57): its inputs were admitted
  if (gw2::recipeRecording()) {
    gw2::RecipeBytes in; // gw2 (#57): only the global shader flags
    in.u32(getGlobalShaderFlag());
    recorded = gw2::recipeVariant(shader->sha1(), variant_digest, pDevice->GetDXMTDevice().metalVersion(),
                                   gw2::RecipeVariantKind::Plain, std::move(in));
  }
  std::string func_name = "cs_" + shader->sha1().string().substr(0, 8) + "_" + variant_digest.string();
  auto proc = [=](const char *func_name, SM50_SHADER_COMMON_DATA *common) -> sm50_bitcode_t  {
    sm50_bitcode_t compile_result = nullptr;
    sm50_error_t sm50_err = nullptr;
    if (SM50Compile(shader->handle(), (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)common, func_name,
                               &compile_result, &sm50_err)) {
      ERR("Failed to compile shader: ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      return nullptr;
    }
    return compile_result;
  };
  return std::make_unique<GeneralShaderCompileTask<decltype(proc)>>(
      pDevice, shader, std::move(proc), func_name, variant_digest, recorded);
};

template <>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *pDevice, ManagedShader shader,
                    ShaderVariantTessellationVertexHull variant) {
  Sha1HashState h;
  h.update(getGlobalShaderFlag());
  h.update(variant.vertex_shader_handle->sha1());
  h.update(variant.index_buffer_format);
  h.update(variant.max_potential_tess_factor);
  if (variant.input_layout_handle)
    h.update(variant.input_layout_handle->sha1());
  auto variant_digest = h.final();
  gw2::recipeVariantUnsupported(); // gw2 (#57): not recorded yet
  std::string func_name = "vshs_" + shader->sha1().string().substr(0, 8) + "_" +  variant_digest.string();
  auto proc = [=](const char *func_name, SM50_SHADER_COMMON_DATA *common) -> sm50_bitcode_t  {
    SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout;
    SM50_SHADER_PSO_TESSELLATOR_DATA pso_tess;
    ia_layout.index_buffer_format = variant.index_buffer_format;
    if (variant.input_layout_handle) {
      ia_layout.slot_mask = variant.input_layout_handle->input_slot_mask();
      ia_layout.num_elements = variant.input_layout_handle->input_layout_element(
          reinterpret_cast<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC **>(&ia_layout.elements)
      );
    } else {
      ia_layout.slot_mask = 0;
      ia_layout.num_elements = 0;
    }
    ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
    ia_layout.next = &pso_tess;
    pso_tess.type = SM50_SHADER_PSO_TESSELLATOR;
    pso_tess.next = common;
    pso_tess.max_potential_tess_factor = variant.max_potential_tess_factor;

    sm50_bitcode_t compile_result = nullptr;
    sm50_error_t sm50_err = nullptr;
    if (SM50CompileTessellationPipelineHull(
            variant.vertex_shader_handle->handle(), shader->handle(), 
            (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&ia_layout, func_name,
            &compile_result, &sm50_err)) {
      ERR("Failed to compile shader: ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      return nullptr;
    }
    return compile_result;
  };
  return std::make_unique<GeneralShaderCompileTask<decltype(proc)>>(
      pDevice, shader, std::move(proc), func_name, variant_digest);
}

template <>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *pDevice, ManagedShader shader,
                    ShaderVariantTessellationDomain variant) {
  Sha1HashState h;
  h.update(getGlobalShaderFlag());
  h.update(variant.hull_shader_handle->sha1());
  h.update(variant.gs_passthrough);
  h.update(variant.rasterization_disabled);
  h.update(variant.max_potential_tess_factor);
  auto variant_digest = h.final();
  gw2::recipeVariantUnsupported(); // gw2 (#57): not recorded yet
  std::string func_name = "ds_" + shader->sha1().string().substr(0, 8) + "_" + variant_digest.string();
  auto proc = [=](const char *func_name, SM50_SHADER_COMMON_DATA *common) -> sm50_bitcode_t  {
    SM50_SHADER_GS_PASS_THROUGH_DATA gs_passthrough;
    SM50_SHADER_PSO_TESSELLATOR_DATA pso_tess;
    gs_passthrough.type = SM50_SHADER_GS_PASS_THROUGH;
    gs_passthrough.DataEncoded = variant.gs_passthrough;
    gs_passthrough.RasterizationDisabled = variant.rasterization_disabled;
    gs_passthrough.next = &pso_tess;
    pso_tess.type = SM50_SHADER_PSO_TESSELLATOR;
    pso_tess.next = common;
    pso_tess.max_potential_tess_factor = variant.max_potential_tess_factor;

    sm50_bitcode_t compile_result = nullptr;
    sm50_error_t sm50_err = nullptr;
    if (SM50CompileTessellationPipelineDomain(
            variant.hull_shader_handle->handle(), shader->handle(),
            (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&gs_passthrough, func_name,
            &compile_result, &sm50_err)) {
      ERR("Failed to compile shader: ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      return nullptr;
    }
    return compile_result;
  };
  return std::make_unique<GeneralShaderCompileTask<decltype(proc)>>(
      pDevice, shader, std::move(proc), func_name, variant_digest);
}

template <>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *pDevice, ManagedShader shader,
                    ShaderVariantVertexStreamOutput variant) {
  Sha1HashState h;
  h.update(getGlobalShaderFlag());
  h.update(((IMTLD3D11StreamOutputLayout *)variant.stream_output_layout_handle)->Digest());
  if (variant.input_layout_handle)
    h.update(variant.input_layout_handle->sha1());
  auto variant_digest = h.final();
  gw2::recipeVariantUnsupported(); // gw2 (#57): not recorded yet
  std::string func_name = "vsso_" + shader->sha1().string().substr(0, 8) + "_" + variant_digest.string();
  auto proc = [=](const char *func_name, SM50_SHADER_COMMON_DATA *common) -> sm50_bitcode_t  {
    SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA data_so;
    SM50_SHADER_IA_INPUT_LAYOUT_DATA data_vertex_pulling;
    data_so.type = SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT;
    data_so.next = common;
    data_so.num_output_slots = 0;
    data_so.num_elements =
        ((IMTLD3D11StreamOutputLayout *)variant.stream_output_layout_handle)
            ->GetStreamOutputElements(
                (MTL_SHADER_STREAM_OUTPUT_ELEMENT_DESC **)&data_so.elements,
                data_so.strides);

    if (variant.input_layout_handle) {
      data_so.next = &data_vertex_pulling;
      data_vertex_pulling.type = SM50_SHADER_IA_INPUT_LAYOUT;
      data_vertex_pulling.next = common;
      data_vertex_pulling.slot_mask =
          variant.input_layout_handle->input_slot_mask();
      data_vertex_pulling.num_elements =
          variant.input_layout_handle->input_layout_element(
              (MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC **)&data_vertex_pulling
                  .elements);
    }

    sm50_bitcode_t compile_result = nullptr;
    sm50_error_t sm50_err = nullptr;
    if (SM50Compile(
            shader->handle(), (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&data_so,
            func_name, &compile_result, &sm50_err)) {
      ERR("Failed to compile shader: ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      return nullptr;
    }
    return compile_result;
  };
  return std::make_unique<GeneralShaderCompileTask<decltype(proc)>>(
      pDevice, shader, std::move(proc), func_name, variant_digest);
};

template <>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *pDevice, ManagedShader shader,
                    ShaderVariantGeometryVertex variant) {
  Sha1HashState h;
  h.update(getGlobalShaderFlag());
  h.update(variant.geometry_shader_handle->sha1());
  h.update(variant.index_buffer_format);
  h.update(variant.strip_topology);
  if (variant.input_layout_handle)
    h.update(variant.input_layout_handle->sha1());
  auto variant_digest = h.final();
  gw2::recipeVariantUnsupported(); // gw2 (#57): not recorded yet
  std::string func_name = "vsgs_" + shader->sha1().string().substr(0, 8) + "_" + variant_digest.string();
  auto proc = [=](const char *func_name, SM50_SHADER_COMMON_DATA *common) -> sm50_bitcode_t  {
    SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout;
    ia_layout.index_buffer_format = variant.index_buffer_format;
    if (variant.input_layout_handle) {
      ia_layout.slot_mask = variant.input_layout_handle->input_slot_mask();
      ia_layout.num_elements =
          variant.input_layout_handle->input_layout_element(
              (MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC **)&ia_layout.elements);
    } else {
      ia_layout.slot_mask = 0;
      ia_layout.num_elements = 0;
    }

    ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
    ia_layout.next = common;

    SM50_SHADER_PSO_GEOMETRY_SHADER_DATA geometry;
    geometry.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
    geometry.next = &ia_layout;
    geometry.strip_topology = variant.strip_topology;

    sm50_bitcode_t compile_result = nullptr;
    sm50_error_t sm50_err = nullptr;
    if (SM50CompileGeometryPipelineVertex(
            shader->handle(), variant.geometry_shader_handle->handle(),
            (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&geometry, func_name,
            &compile_result, &sm50_err)) {
      ERR("Failed to compile shader: ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      return nullptr;
    }
    return compile_result;
  };
  return std::make_unique<GeneralShaderCompileTask<decltype(proc)>>(
      pDevice, shader, std::move(proc), func_name, variant_digest);
}

template <>
std::unique_ptr<CompiledShader>
CreateVariantShader(MTLD3D11Device *pDevice, ManagedShader shader,
                    ShaderVariantGeometry variant) {
  Sha1HashState h;
  h.update(getGlobalShaderFlag());
  h.update(variant.vertex_shader_handle->sha1());
  h.update(variant.strip_topology);
  auto variant_digest = h.final();
  gw2::recipeVariantUnsupported(); // gw2 (#57): not recorded yet
  std::string func_name = "gs_" + shader->sha1().string().substr(0, 8) + "_" + variant_digest.string();
  auto proc = [=](const char *func_name, SM50_SHADER_COMMON_DATA *common) -> sm50_bitcode_t  {
    SM50_SHADER_PSO_GEOMETRY_SHADER_DATA geometry;
    geometry.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
    geometry.next = common;
    geometry.strip_topology = variant.strip_topology;

    sm50_bitcode_t compile_result = nullptr;
    sm50_error_t sm50_err = nullptr;
    if (SM50CompileGeometryPipelineGeometry(
            variant.vertex_shader_handle->handle(), shader->handle(),
            (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&geometry, func_name,
            &compile_result, &sm50_err)) {
      ERR("Failed to compile shader: ", SM50GetErrorMessageString(sm50_err));
      SM50FreeError(sm50_err);
      return nullptr;
    }
    return compile_result;
  };
  return std::make_unique<GeneralShaderCompileTask<decltype(proc)>>(
      pDevice, shader, std::move(proc), func_name, variant_digest);
}

} // namespace dxmt