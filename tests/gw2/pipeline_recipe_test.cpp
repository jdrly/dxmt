// gw2-test: pure
// gw2-build: clang -c -O1 src/util/sha1/sha1.c -o ${OUT}_sha1.o
// gw2-build: clang++ -std=c++20 -O1 -Itests/gw2/shim -Itests/gw2 -Isrc/dxmt -Isrc/util -Isrc/winemetal tests/gw2/pipeline_recipe_test.cpp src/dxmt/dxmt_compile_recipe.cpp src/util/sha1/sha1_util.cpp ${OUT}_sha1.o -o $OUT
//
// gw2 (C3, architecture council §1): native test of the pipeline recipe (src/dxmt/dxmt_compile_recipe.cpp).
// No Metal, no Wine.
//
// - Round trip: a descriptor as DXMT fills it (every recipe field set at random over its range, the
//   rest from InitializeRenderPipelineInfo, function handles set) equals a fresh initialized descriptor
//   with applyRecipe(recipeFromInfo(it)) and the same functions, byte for byte. That is what makes the
//   builder (which builds from the recipe) build exactly what DXMT built before.
// - The serializer is #57's, byte for byte (tests/gw2/fixtures/w1_recipe_serializer.hpp, the old code):
//   keys already recorded in recipes_1.db stay valid.
// - Shapes: attachment holes, depth-only, fragment-less, rasterization off.
// - The key is fixed for a fixed recipe (a golden value), and changes when any field changes.
// - A recording fake builder receives the recipe and functions it is given.

#include "dxmt_compile_recipe.hpp"
#include "fixtures/w1_recipe_serializer.hpp"
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace dxmt;

// The bridge calls WMT::Reference makes on empty handles here (no Metal object exists in this test).
extern "C" void
NSObject_retain(obj_handle_t) {}
extern "C" void
NSObject_release(obj_handle_t) {}

static int failures = 0;
#define CHECK(cond, ...)                                                                                          \
  do {                                                                                                            \
    if (!(cond)) {                                                                                                \
      failures++;                                                                                                 \
      std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                 \
      std::printf(__VA_ARGS__);                                                                                   \
      std::printf("\n");                                                                                          \
    }                                                                                                             \
  } while (0)

static void
fresh(WMTRenderPipelineInfo &info) {
  std::memset(&info, 0, sizeof(info)); // padding too, so descriptors compare byte for byte
  WMT::InitializeRenderPipelineInfo(info);
}

// A descriptor as DXMT's graphics pipeline fills it, at random.
static void
randomInfo(std::mt19937 &rng, WMTRenderPipelineInfo &info, int shape) {
  fresh(info);
  auto pick = [&](uint32_t n) { return uint32_t(rng() % n); };
  unsigned rtvs = shape == 1 ? 0 : 1 + pick(8); // shape 1: depth-only
  for (unsigned i = 0; i < rtvs; i++) {
    auto &c = info.colors[i];
    if (shape == 2 && i % 2)
      continue; // attachment holes
    c.pixel_format = WMTPixelFormat(pick(600));
    c.rgb_blend_operation = WMTBlendOperation(pick(5));
    c.alpha_blend_operation = WMTBlendOperation(pick(5));
    c.src_rgb_blend_factor = WMTBlendFactor(pick(19));
    c.dst_rgb_blend_factor = WMTBlendFactor(pick(19));
    c.src_alpha_blend_factor = WMTBlendFactor(pick(19));
    c.dst_alpha_blend_factor = WMTBlendFactor(pick(19));
    c.write_mask = uint8_t(pick(16));
    c.blending_enabled = pick(2);
  }
  info.alpha_to_coverage_enabled = pick(2);
  info.logic_operation_enabled = pick(2);
  info.logic_operation = WMTLogicOperation(pick(16));
  info.rasterization_enabled = shape == 3 ? false : true; // shape 3: rasterization off
  info.raster_sample_count = uint8_t(1u << pick(4));
  if (pick(3)) {
    info.depth_pixel_format = WMTPixelFormat(250 + pick(10));
    if (pick(2))
      info.stencil_pixel_format = info.depth_pixel_format;
  }
  info.vertex_function = 0x1000 + pick(1000);
  info.fragment_function = shape == 4 ? NULL_OBJECT_HANDLE : 0x9000 + pick(1000); // shape 4: fragment-less
  info.immutable_vertex_buffers = (1 << 16) | (1 << 29) | (1 << 30);
  info.immutable_fragment_buffers = (1 << 29) | (1 << 30);
  info.input_primitive_topology = WMTPrimitiveTopologyClass(pick(4));
  if (pick(4) == 0) {
    // fields DXMT's plain pipelines leave at their defaults, varied anyway: the recipe keeps them all
    info.tessellation_partition_mode = WMTTessellationPartitionMode(pick(4));
    info.max_tessellation_factor = uint8_t(pick(65));
    info.tessellation_output_winding_order = WMTWinding(pick(2));
    info.tessellation_factor_step = WMTTessellationFactorStepFunction(pick(4));
    info.support_indirect_command_buffers = pick(2);
  }
}

static Sha1Digest
digest(uint32_t tag) {
  return Sha1HashState::compute(&tag, sizeof(tag));
}

// A recording fake builder: what it was asked to build.
struct RecordingBuilder final : compile::PipelineBuilder {
  std::vector<std::string> keys;
  std::vector<obj_handle_t> vs, ps;
  WMT::Reference<WMT::RenderPipelineState>
  build(const compile::PipelineRecipe &recipe, WMT::Function v, WMT::Function p, WMT::Reference<WMT::Error> &) override {
    keys.push_back(recipe.vs ? compile::recipeKey(recipe).string() : "-");
    vs.push_back(v.handle);
    ps.push_back(p.handle);
    return {};
  }
};

int
main() {
  std::mt19937 rng(20261104);
  const char *shapes[] = {"general", "depth-only", "attachment holes", "rasterization off", "fragment-less"};
  int roundtrips = 0, same_bytes = 0, n = 0;
  for (int shape = 0; shape < 5; shape++) {
    for (int i = 0; i < 2000; i++, n++) {
      WMTRenderPipelineInfo info, back;
      randomInfo(rng, info, shape);
      // round trip
      auto recipe = compile::recipeFromInfo(info);
      fresh(back);
      compile::applyRecipe(recipe, back);
      back.vertex_function = info.vertex_function;
      back.fragment_function = info.fragment_function;
      bool rt = std::memcmp(&info, &back, sizeof(info)) == 0;
      roundtrips += rt;
      if (!rt && failures < 5)
        CHECK(rt, "round trip (%s, #%d)", shapes[shape], i);
      // the serializer is #57's
      Sha1Digest vs = digest(i), vsv = digest(i + 7), ps = digest(i + 11), psv = digest(i + 13);
      bool has_ps = info.fragment_function != NULL_OBJECT_HANDLE;
      recipe.metal_version = 310 + 10 * (i % 2);
      recipe.vs = compile::VariantKey{vs, vsv};
      if (has_ps)
        recipe.ps = compile::VariantKey{ps, psv};
      gw2::RecipeBytes now, before;
      compile::serialize(recipe, now);
      legacy_w1::serialize(vs, vsv, has_ps ? &ps : nullptr, has_ps ? &psv : nullptr, recipe.metal_version, info, before);
      bool same = now.data == before.data;
      same_bytes += same;
      if (!same && failures < 5)
        CHECK(same, "serialization differs from #57's (%s, #%d)", shapes[shape], i);
    }
  }
  CHECK(roundtrips == n, "round trips %d/%d", roundtrips, n);
  CHECK(same_bytes == n, "#57's bytes %d/%d", same_bytes, n);
  std::printf("%s round trip: %d/%d descriptors back byte for byte (general, depth-only, holes, raster off, "
              "fragment-less)\n",
              roundtrips == n ? "ok  " : "FAIL", roundtrips, n);
  std::printf("%s serializer: %d/%d recipes serialize to #57's bytes\n", same_bytes == n ? "ok  " : "FAIL",
              same_bytes, n);

  // a fixed recipe: its key is fixed across runs and builds
  WMTRenderPipelineInfo info;
  fresh(info);
  info.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
  info.depth_pixel_format = WMTPixelFormatDepth32Float;
  info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
  info.immutable_vertex_buffers = (1 << 16) | (1 << 29) | (1 << 30);
  info.immutable_fragment_buffers = (1 << 29) | (1 << 30);
  auto base = compile::recipeFromInfo(info);
  base.metal_version = 320;
  base.vs = compile::VariantKey{digest(1), digest(2)};
  base.ps = compile::VariantKey{digest(3), digest(4)};
  auto golden = compile::recipeKey(base).string();
  std::printf("     golden key: %s\n", golden.c_str());
  // fixed at C3: a change here means recorded recipes no longer match their keys (bump kRecipeSchema)
  const char *kGolden = "26644aa38b254e65b469885faf0e89d0b5720850";
  CHECK(golden == kGolden, "golden key changed: %s", golden.c_str());

  // every field changes the key
  std::vector<std::pair<const char *, std::function<void(compile::PipelineRecipe &)>>> edits = {
      {"metal_version", [](auto &r) { r.metal_version = 310; }},
      {"vs.shader", [](auto &r) { r.vs->shader = digest(9); }},
      {"vs.variant", [](auto &r) { r.vs->variant = digest(9); }},
      {"ps absent", [](auto &r) { r.ps.reset(); }},
      {"ps.shader", [](auto &r) { r.ps->shader = digest(9); }},
      {"ps.variant", [](auto &r) { r.ps->variant = digest(9); }},
      {"colors[7].pixel_format", [](auto &r) { r.colors[7].pixel_format = WMTPixelFormatRGBA8Unorm; }},
      {"colors[0].rgb_blend_operation", [](auto &r) { r.colors[0].rgb_blend_operation = WMTBlendOperationMax; }},
      {"colors[0].alpha_blend_operation", [](auto &r) { r.colors[0].alpha_blend_operation = WMTBlendOperationMin; }},
      {"colors[0].src_rgb_blend_factor", [](auto &r) { r.colors[0].src_rgb_blend_factor = WMTBlendFactorZero; }},
      {"colors[0].dst_rgb_blend_factor", [](auto &r) { r.colors[0].dst_rgb_blend_factor = WMTBlendFactorOne; }},
      {"colors[0].src_alpha_blend_factor", [](auto &r) { r.colors[0].src_alpha_blend_factor = WMTBlendFactorZero; }},
      {"colors[0].dst_alpha_blend_factor", [](auto &r) { r.colors[0].dst_alpha_blend_factor = WMTBlendFactorOne; }},
      {"colors[0].write_mask", [](auto &r) { r.colors[0].write_mask = 1; }},
      {"colors[0].blending_enabled", [](auto &r) { r.colors[0].blending_enabled = true; }},
      {"alpha_to_coverage_enabled", [](auto &r) { r.alpha_to_coverage_enabled = true; }},
      {"logic_operation_enabled", [](auto &r) { r.logic_operation_enabled = true; }},
      {"logic_operation", [](auto &r) { r.logic_operation = WMTLogicOperation(3); }},
      {"rasterization_enabled", [](auto &r) { r.rasterization_enabled = false; }},
      {"raster_sample_count", [](auto &r) { r.raster_sample_count = 4; }},
      {"depth_pixel_format", [](auto &r) { r.depth_pixel_format = WMTPixelFormatInvalid; }},
      {"stencil_pixel_format", [](auto &r) { r.stencil_pixel_format = WMTPixelFormat(255); }},
      {"immutable_vertex_buffers", [](auto &r) { r.immutable_vertex_buffers = 0; }},
      {"immutable_fragment_buffers", [](auto &r) { r.immutable_fragment_buffers = 0; }},
      {"input_primitive_topology", [](auto &r) { r.input_primitive_topology = WMTPrimitiveTopologyClassLine; }},
      {"tessellation_partition_mode", [](auto &r) { r.tessellation_partition_mode = WMTTessellationPartitionMode(1); }},
      {"max_tessellation_factor", [](auto &r) { r.max_tessellation_factor = 64; }},
      {"tessellation_output_winding_order", [](auto &r) { r.tessellation_output_winding_order = WMTWinding(1); }},
      {"tessellation_factor_step", [](auto &r) { r.tessellation_factor_step = WMTTessellationFactorStepFunction(1); }},
      {"support_indirect_command_buffers", [](auto &r) { r.support_indirect_command_buffers = true; }},
  };
  std::set<std::string> keys{golden};
  int changed = 0;
  for (auto &[name, edit] : edits) {
    auto r = base;
    edit(r);
    auto k = compile::recipeKey(r).string();
    bool differs = keys.insert(k).second;
    changed += differs;
    CHECK(differs, "changing %s left the key as another recipe's", name);
  }
  std::printf("%s key: each of %zu field changes gives a new key\n", changed == int(edits.size()) ? "ok  " : "FAIL",
              edits.size());

  // the builder port: a recording fake gets the recipe and functions it's given
  RecordingBuilder fake;
  WMT::Reference<WMT::Error> err;
  compile::PipelineBuilder &builder = fake;
  builder.build(base, WMT::Function{0x1234}, WMT::Function{}, err);
  bool fake_ok = fake.keys.size() == 1 && fake.keys[0] == golden && fake.vs[0] == 0x1234 && fake.ps[0] == 0;
  CHECK(fake_ok, "the fake builder saw the recipe and functions");
  std::printf("%s builder port: a recording fake receives the recipe and functions\n", fake_ok ? "ok  " : "FAIL");

  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
