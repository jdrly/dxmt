#include "dxmt_compile_recipe.hpp"

// gw2 (C3): see dxmt_compile_recipe.hpp. Pure code (no Metal calls): tests/gw2/pipeline_recipe_test.cpp
// builds it natively.

namespace dxmt::compile {

PipelineRecipe
recipeFromInfo(const WMTRenderPipelineInfo &info) {
  PipelineRecipe r;
  for (unsigned i = 0; i < 8; i++)
    r.colors[i] = info.colors[i];
  r.alpha_to_coverage_enabled = info.alpha_to_coverage_enabled;
  r.logic_operation_enabled = info.logic_operation_enabled;
  r.logic_operation = info.logic_operation;
  r.rasterization_enabled = info.rasterization_enabled;
  r.raster_sample_count = info.raster_sample_count;
  r.depth_pixel_format = info.depth_pixel_format;
  r.stencil_pixel_format = info.stencil_pixel_format;
  r.immutable_vertex_buffers = info.immutable_vertex_buffers;
  r.immutable_fragment_buffers = info.immutable_fragment_buffers;
  r.input_primitive_topology = info.input_primitive_topology;
  r.tessellation_partition_mode = info.tessellation_partition_mode;
  r.max_tessellation_factor = info.max_tessellation_factor;
  r.tessellation_output_winding_order = info.tessellation_output_winding_order;
  r.tessellation_factor_step = info.tessellation_factor_step;
  r.support_indirect_command_buffers = info.support_indirect_command_buffers;
  return r;
}

void
applyRecipe(const PipelineRecipe &r, WMTRenderPipelineInfo &info) {
  for (unsigned i = 0; i < 8; i++)
    info.colors[i] = r.colors[i];
  info.alpha_to_coverage_enabled = r.alpha_to_coverage_enabled;
  info.logic_operation_enabled = r.logic_operation_enabled;
  info.logic_operation = r.logic_operation;
  info.rasterization_enabled = r.rasterization_enabled;
  info.raster_sample_count = r.raster_sample_count;
  info.depth_pixel_format = r.depth_pixel_format;
  info.stencil_pixel_format = r.stencil_pixel_format;
  info.immutable_vertex_buffers = r.immutable_vertex_buffers;
  info.immutable_fragment_buffers = r.immutable_fragment_buffers;
  info.input_primitive_topology = r.input_primitive_topology;
  info.tessellation_partition_mode = r.tessellation_partition_mode;
  info.max_tessellation_factor = r.max_tessellation_factor;
  info.tessellation_output_winding_order = r.tessellation_output_winding_order;
  info.tessellation_factor_step = r.tessellation_factor_step;
  info.support_indirect_command_buffers = r.support_indirect_command_buffers;
}

void
serialize(const PipelineRecipe &r, gw2::RecipeBytes &b) {
  // explicit fields only, in a fixed order (#57's format; docs/gw2/README.md, RECORD)
  b.u32(gw2::kRecipeSchema);
  b.u32(r.metal_version);
  b.digest(r.vs->shader);
  b.digest(r.vs->variant);
  b.u8(r.ps.has_value());
  if (r.ps) {
    b.digest(r.ps->shader);
    b.digest(r.ps->variant);
  }
  for (auto &c : r.colors) {
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
  b.u8(r.alpha_to_coverage_enabled);
  b.u8(r.logic_operation_enabled);
  b.u32(r.logic_operation);
  b.u8(r.rasterization_enabled);
  b.u8(r.raster_sample_count);
  b.u32(r.depth_pixel_format);
  b.u32(r.stencil_pixel_format);
  b.u32(r.immutable_vertex_buffers);
  b.u32(r.immutable_fragment_buffers);
  b.u32(r.input_primitive_topology);
  b.u32(r.tessellation_partition_mode);
  b.u8(r.max_tessellation_factor);
  b.u32(r.tessellation_output_winding_order);
  b.u32(r.tessellation_factor_step);
  b.u8(r.support_indirect_command_buffers);
}

Sha1Digest
recipeKey(const PipelineRecipe &r) {
  gw2::RecipeBytes b;
  serialize(r, b);
  return Sha1HashState::compute(b.data.data(), b.data.size());
}

} // namespace dxmt::compile
