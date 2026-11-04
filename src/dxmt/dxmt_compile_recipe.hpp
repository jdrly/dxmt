#pragma once

// gw2 (C3, architecture council §1): the pipeline recipe and the pipeline builder.
//
// A pipeline recipe is the complete, explicit input a plain graphics pipeline is built from: the
// vertex and pixel shader variants' identities (when known), the Metal target, and every field of
// the WMTRenderPipelineInfo DXMT gives Metal, except the function handles (live objects) and the
// binary-archive fields (never set where pipelines are built). It is built from the exact descriptor
// (recipeFromInfo); applied to an initialized descriptor it gives that descriptor back
// (applyRecipe). One canonical serializer (serialize, W1's #57 format byte for byte) and its SHA-1
// (recipeKey) serve both the recorder (#57) and, later, stable keys (C4) and warming (W2/W3).
//
// The builder is a port: Metal3PipelineBuilder builds through the device as DXMT always did; tests
// use a recording fake; MTL4Compiler (#34) is a later adapter. The compile service never sees Metal.

#include "Metal.hpp"
#include "dxmt_gw2_recipe.hpp"
#include "sha1/sha1_util.hpp"
#include <cstdint>
#include <optional>

namespace dxmt::compile {

// A shader variant's identity: the shader's SHA-1 and the variant digest (the AIR cache's key).
struct VariantKey {
  Sha1Digest shader;
  Sha1Digest variant;
};

struct PipelineRecipe {
  uint32_t metal_version = 0;   // SM50_SHADER_METAL_VERSION, the variants' translation target
  std::optional<VariantKey> vs; // unknown: the recipe can be built but not recorded
  std::optional<VariantKey> ps; // none: no pixel shader (or unknown, see `recordable`)
  WMTColorAttachmentBlendInfo colors[8];
  bool alpha_to_coverage_enabled = false;
  bool logic_operation_enabled = false;
  WMTLogicOperation logic_operation = WMTLogicOperationClear;
  bool rasterization_enabled = true;
  uint8_t raster_sample_count = 1;
  WMTPixelFormat depth_pixel_format = WMTPixelFormatInvalid;
  WMTPixelFormat stencil_pixel_format = WMTPixelFormatInvalid;
  uint32_t immutable_vertex_buffers = 0;
  uint32_t immutable_fragment_buffers = 0;
  WMTPrimitiveTopologyClass input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;
  WMTTessellationPartitionMode tessellation_partition_mode = WMTTessellationPartitionModePow2;
  uint8_t max_tessellation_factor = 16;
  WMTWinding tessellation_output_winding_order = WMTWindingClockwise;
  WMTTessellationFactorStepFunction tessellation_factor_step = WMTTessellationFactorStepFunctionConstant;
  bool support_indirect_command_buffers = false;
};

// Every recipe field from the descriptor (identities and the target are the caller's).
PipelineRecipe recipeFromInfo(const WMTRenderPipelineInfo &info);
// The recipe's fields onto a descriptor (InitializeRenderPipelineInfo'd by the caller); functions and
// archive fields are left as they are.
void applyRecipe(const PipelineRecipe &recipe, WMTRenderPipelineInfo &info);
// The canonical serialization (#57's, byte for byte): needs the vertex identity.
void serialize(const PipelineRecipe &recipe, gw2::RecipeBytes &out);
// SHA-1 of the serialization: the recipe's stable key.
Sha1Digest recipeKey(const PipelineRecipe &recipe);

// Builds a pipeline from a recipe and its functions (`ps` empty: no pixel shader).
class PipelineBuilder {
public:
  virtual ~PipelineBuilder() = default;
  virtual WMT::Reference<WMT::RenderPipelineState>
  build(const PipelineRecipe &recipe, WMT::Function vs, WMT::Function ps, WMT::Reference<WMT::Error> &error) = 0;
};

// The production adapter: the device's newRenderPipelineState, as DXMT built pipelines before C3.
class Metal3PipelineBuilder final : public PipelineBuilder {
public:
  explicit Metal3PipelineBuilder(WMT::Device device) : device_(device) {}
  WMT::Reference<WMT::RenderPipelineState>
  build(const PipelineRecipe &recipe, WMT::Function vs, WMT::Function ps, WMT::Reference<WMT::Error> &error) override;

private:
  WMT::Device device_;
};

} // namespace dxmt::compile
