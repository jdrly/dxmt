// gw2 (C3): #57's recipe serialization as it was before the pipeline recipe (src/dxmt/dxmt_gw2_recipe.cpp
// at 3ea524a, recipePipeline's serialization unchanged; the recording calls around it removed), for
// pipeline_recipe_test.cpp: compile::serialize must give the same bytes, so recorded keys stay valid.
#pragma once
#include "Metal.hpp"
#include "dxmt_gw2_recipe.hpp"

namespace legacy_w1 {
using namespace dxmt;
using namespace dxmt::gw2;

inline void
serialize(const Sha1Digest &vs_shader, const Sha1Digest &vs_variant, const Sha1Digest *ps_shader,
               const Sha1Digest *ps_variant, uint32_t metal_version, const WMTRenderPipelineInfo &info, RecipeBytes &out) {
  // The canonical serialization: explicit fields only, in a fixed order.
  RecipeBytes &b = out;
  b.u32(kRecipeSchema);
  b.u32(metal_version);
  b.digest(vs_shader);
  b.digest(vs_variant);
  b.u8(ps_shader && ps_variant);
  if (ps_shader && ps_variant) {
    b.digest(*ps_shader);
    b.digest(*ps_variant);
  }
  for (auto &c : info.colors) {
    b.u32(c.pixel_format);
    b.u32(c.rgb_blend_operation);
    b.u32(c.alpha_blend_operation);
    b.u32(c.src_rgb_blend_factor);
    b.u32(c.dst_rgb_blend_factor);
    b.u32(c.src_alpha_blend_factor);
    b.u32(c.dst_alpha_blend_factor);
    b.u8(c.write_mask);
    b.u8(c.blending_enabled);
  }
  b.u8(info.alpha_to_coverage_enabled);
  b.u8(info.logic_operation_enabled);
  b.u32(info.logic_operation);
  b.u8(info.rasterization_enabled);
  b.u8(info.raster_sample_count);
  b.u32(info.depth_pixel_format);
  b.u32(info.stencil_pixel_format);
  b.u32(info.immutable_vertex_buffers);
  b.u32(info.immutable_fragment_buffers);
  b.u32(info.input_primitive_topology);
  b.u32(info.tessellation_partition_mode);
  b.u8(info.max_tessellation_factor);
  b.u32(info.tessellation_output_winding_order);
  b.u32(info.tessellation_factor_step);
  b.u8(info.support_indirect_command_buffers);
}

} // namespace legacy_w1
