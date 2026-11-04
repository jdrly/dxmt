// gw2-test: metal
// gw2-build: tools/gw2/build-pso-archive-tool.zsh ${OUT}_tool_x86_64 x86_64
// gw2-build: tools/gw2/build-pso-archive-tool.zsh ${OUT}_tool_arm64 arm64
// gw2-build: tools/gw2/build-pso-archive-tool.zsh ${OUT}_tool_testing x86_64 testing
// gw2-build: clang -c -O1 -mmacosx-version-min=26.0 src/util/sha1/sha1.c -o ${OUT}_sha1.o
// gw2-build: clang++ -std=c++20 -fobjc-arc -O1 -Wall -Wextra -mmacosx-version-min=26.0 -Itests/gw2/shim -Isrc/winemetal -Isrc/dxmt -Isrc/util tests/gw2/pso_archive_tool_test.mm src/dxmt/dxmt_compile_recipe.cpp src/util/sha1/sha1_util.cpp ${OUT}_sha1.o -framework Metal -framework Foundation -lsqlite3 -o ${OUT}_gen
// gw2-run: tests/gw2/pso_archive_tool.zsh ${OUT}_gen ${OUT}_tool_x86_64 ${OUT}_tool_arm64 ${OUT}_tool_testing "$T/pso_archive_tool"
//
// gw2 (#111): the synthetic recipes for the native PSO archive tool's test (tests/gw2/pso_archive_tool.zsh).
// No real recipes_1.db exists yet (nobody has played since W1), so this writes one from real AIR: GW2's
// translated shaders in an isolated copy of DXMT's AIR cache (lane C's work/dxmt-squeeze/native/w0/).
//
//   pso_archive_tool_test_gen <AIR copy> <recipes_1.db to write> [pairs]
//
// It reads the AIR rows in rowid order (read-only), keeps each library whose one function is named as
// DXMT names variants (vs_/ps_ + shader SHA-1 prefix + variant digest), and pairs pixel functions with
// vertex functions that link (tried on the device). Then it writes recipes through C3's one canonical
// serializer (dxmt_compile_recipe.cpp), keyed 'R' + recipeKey as W1 writes them, in these shapes:
//   full    VS/PS, one BGRA8 target, Depth32Float, no blending
//   blend   VS/PS, alpha blending on the target, a write mask without alpha
//   sparse  VS/PS, targets in slots 0 and 2, slot 1 empty
//   msaa    VS/PS, 4 samples
//   novs    fragment-less: the VS alone with a depth target (a depth-only pass)
// (Not rasterization off: Metal requires a void vertex function there, and GW2's vertex shaders all
// return a position.)
// Only recipes that build directly on the device are written (a check in this test only; the tool
// builds through the bridge). It prints "RECIPES <n> <shape>=<n>..." for the driver.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "dxmt_compile_recipe.hpp"
#include <cstdio>
#include <map>
#include <sqlite3.h>
#include <string>
#include <vector>

using namespace dxmt;

namespace {

struct Fn {
  compile::VariantKey key;
  id<MTLFunction> fn;
  bool pixel;
};

std::string
hex(const uint8_t *p, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

// what the recipe asks of Metal, for the check before writing it (the bridge's conversion is the tool's)
MTLRenderPipelineDescriptor *
checkDescriptor(const compile::PipelineRecipe &r, id<MTLFunction> vs, id<MTLFunction> ps) {
  auto d = [MTLRenderPipelineDescriptor new];
  d.vertexFunction = vs;
  d.fragmentFunction = ps;
  for (int i = 0; i < 8; i++) {
    d.colorAttachments[i].pixelFormat = MTLPixelFormat(r.colors[i].pixel_format);
    d.colorAttachments[i].blendingEnabled = r.colors[i].blending_enabled;
    d.colorAttachments[i].writeMask = MTLColorWriteMask(r.colors[i].write_mask);
    d.colorAttachments[i].sourceRGBBlendFactor = MTLBlendFactor(r.colors[i].src_rgb_blend_factor);
    d.colorAttachments[i].destinationRGBBlendFactor = MTLBlendFactor(r.colors[i].dst_rgb_blend_factor);
    d.colorAttachments[i].sourceAlphaBlendFactor = MTLBlendFactor(r.colors[i].src_alpha_blend_factor);
    d.colorAttachments[i].destinationAlphaBlendFactor = MTLBlendFactor(r.colors[i].dst_alpha_blend_factor);
  }
  d.depthAttachmentPixelFormat = MTLPixelFormat(r.depth_pixel_format);
  d.rasterSampleCount = r.raster_sample_count;
  d.rasterizationEnabled = r.rasterization_enabled;
  return d;
}

compile::PipelineRecipe
baseRecipe(const Fn &vs, const Fn *ps) {
  WMTRenderPipelineInfo info;
  WMT::InitializeRenderPipelineInfo(info);
  auto r = compile::recipeFromInfo(info);
  r.metal_version = WMTMetal320;
  r.vs = vs.key;
  if (ps)
    r.ps = ps->key;
  r.depth_pixel_format = WMTPixelFormatDepth32Float;
  r.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
  if (ps) {
    r.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
    r.colors[0].write_mask = 0xf;
  }
  return r;
}

} // namespace

int
main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 3) {
      fprintf(stderr, "usage: %s <AIR copy> <recipes db> [pairs]\n", argv[0]);
      return 2;
    }
    int want_pairs = argc > 3 ? atoi(argv[3]) : 8;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      printf("SKIP: no Metal device\n");
      return 77;
    }
    sqlite3 *air = nullptr;
    if (sqlite3_open_v2(argv[1], &air, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      fprintf(stderr, "cannot open %s\n", argv[1]);
      return 2;
    }
    sqlite3_stmt *st = nullptr;
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT key, value FROM cache_%d ORDER BY rowid;", 26);
    if (sqlite3_prepare_v2(air, sql, -1, &st, nullptr) != SQLITE_OK) {
      fprintf(stderr, "no cache_26 table in %s\n", argv[1]);
      return 2;
    }
    std::vector<Fn> vss, pss;
    while (sqlite3_step(st) == SQLITE_ROW && (vss.size() < 48 || pss.size() < 48)) {
      if (sqlite3_column_bytes(st, 0) != 40)
        continue;
      Fn f;
      memcpy(f.key.shader.data, sqlite3_column_blob(st, 0), 20);
      memcpy(f.key.variant.data, (const uint8_t *)sqlite3_column_blob(st, 0) + 20, 20);
      dispatch_data_t data = dispatch_data_create(sqlite3_column_blob(st, 1), sqlite3_column_bytes(st, 1), nil,
                                                  DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      NSError *err = nil;
      id<MTLLibrary> lib = [device newLibraryWithData:data error:&err];
      if (!lib)
        continue;
      std::string tail = hex(f.key.shader.data, 4) + "_" + hex(f.key.variant.data, 20);
      for (NSString *name in lib.functionNames) {
        std::string n = name.UTF8String;
        if (n == "vs_" + tail || n == "ps_" + tail) {
          f.pixel = n[0] == 'p';
          f.fn = [lib newFunctionWithName:name];
          (f.pixel ? pss : vss).push_back(f);
        }
      }
    }
    sqlite3_finalize(st);
    sqlite3_close(air);
    if (vss.empty()) {
      fprintf(stderr, "no vertex functions named as DXMT names them in %s\n", argv[1]);
      return 2;
    }

    std::vector<std::pair<compile::PipelineRecipe, std::string>> recipes;
    std::map<std::string, int> shapes;
    auto add = [&](const compile::PipelineRecipe &r, const char *shape, id<MTLFunction> vs, id<MTLFunction> ps) {
      NSError *err = nil;
      id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:checkDescriptor(r, vs, ps)
                                                                              error:&err];
      if (!pso)
        return false;
      recipes.push_back({r, shape});
      shapes[shape]++;
      return true;
    };
    // pairs that link: each pixel function with the first vertex function that builds with it
    int pairs = 0;
    for (auto &ps : pss) {
      if (pairs >= want_pairs)
        break;
      for (auto &vs : vss) {
        auto r = baseRecipe(vs, &ps);
        if (!add(r, "full", vs.fn, ps.fn))
          continue;
        pairs++;
        auto b = r;
        b.colors[0].blending_enabled = true;
        b.colors[0].src_rgb_blend_factor = WMTBlendFactorSourceAlpha;
        b.colors[0].dst_rgb_blend_factor = WMTBlendFactorOneMinusSourceAlpha;
        b.colors[0].src_alpha_blend_factor = WMTBlendFactorOne;
        b.colors[0].dst_alpha_blend_factor = WMTBlendFactorZero;
        b.colors[0].write_mask = 0xe;
        add(b, "blend", vs.fn, ps.fn);
        auto s = r;
        s.colors[2].pixel_format = WMTPixelFormatRGBA16Float;
        s.colors[2].write_mask = 0xf;
        add(s, "sparse", vs.fn, ps.fn);
        auto m = r;
        m.raster_sample_count = 4;
        add(m, "msaa", vs.fn, ps.fn);
        break;
      }
    }
    for (size_t i = 0; i < vss.size() && i < size_t(want_pairs); i++) {
      add(baseRecipe(vss[i], nullptr), "novs", vss[i].fn, nil);
    }

    sqlite3 *out = nullptr;
    if (sqlite3_open_v2(argv[2], &out, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
      return 2;
    snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS cache_%u (key BLOB PRIMARY KEY, value BLOB NOT NULL);",
             gw2::kRecipeSchema);
    sqlite3_exec(out, sql, nullptr, nullptr, nullptr);
    snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO cache_%u (key, value) VALUES (?, ?);", gw2::kRecipeSchema);
    sqlite3_prepare_v2(out, sql, -1, &st, nullptr);
    sqlite3_exec(out, "BEGIN;", nullptr, nullptr, nullptr);
    size_t written = 0;
    for (auto &[r, shape] : recipes) {
      gw2::RecipeBytes b;
      compile::serialize(r, b);
      auto key = compile::recipeKey(r);
      uint8_t k[21] = {'R'};
      memcpy(k + 1, key.data, 20);
      sqlite3_reset(st);
      sqlite3_bind_blob(st, 1, k, sizeof(k), SQLITE_TRANSIENT);
      sqlite3_bind_blob(st, 2, b.data.data(), int(b.data.size()), SQLITE_TRANSIENT);
      if (sqlite3_step(st) != SQLITE_DONE)
        return 2;
      if (sqlite3_changes(out)) // the plan's index: rowid order
        printf("RECIPE %zu %s %s\n", written++, shape.c_str(), key.string().c_str());
    }
    sqlite3_exec(out, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_finalize(st);
    sqlite3_close(out);
    printf("RECIPES %zu", written);
    for (auto &[shape, n] : shapes)
      printf(" %s=%d", shape.c_str(), n);
    printf(" (from %zu vertex and %zu pixel functions)\n", vss.size(), pss.size());
  }
  return 0;
}
