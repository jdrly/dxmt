#include "dxmt_compile_recipe.hpp"

// gw2 (C3): the production pipeline builder (Metal 3 device), kept apart from the pure recipe code.

namespace dxmt::compile {

WMT::Reference<WMT::RenderPipelineState>
Metal3PipelineBuilder::build(const PipelineRecipe &recipe, WMT::Function vs, WMT::Function ps,
                             WMT::Reference<WMT::Error> &error) {
  WMTRenderPipelineInfo info;
  WMT::InitializeRenderPipelineInfo(info);
  applyRecipe(recipe, info);
  info.vertex_function = vs;
  info.fragment_function = ps; // empty: no pixel shader (as InitializeRenderPipelineInfo leaves it)
  return device_.newRenderPipelineState(info, error);
}

} // namespace dxmt::compile
