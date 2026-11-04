#pragma once

// gw2 (#57): shader warming, stage W1. Records, durably and per session, what each pipeline was
// built from, independent of D3D11 objects, so later sessions can prepare shaders and pipelines
// before GW2 asks for them (W2/W3):
//
// - the DXBC of every shader, by its SHA-1 (the AIR can be regenerated after an airconv change);
// - each shader variant's translation inputs, by (shader SHA-1, variant digest, Metal target);
// - each graphics pipeline's recipe: the vertex and pixel shader identities and every field of the
//   WMTRenderPipelineInfo handed to Metal, serialized explicitly (never pointers or padding), keyed
//   by the SHA-1 of that serialization.
//
// Into $(getconf DARWIN_USER_CACHE_DIR)dxmt/<exe>/recipes_<schema>.db through the bridge's cache
// writer, from one lowest-priority writer thread. The callers never wait on the recorder: they
// copy, check a fixed-size lock-free table of what this process has seen, and push onto a bounded
// lock-free list. The file has a byte budget; rows already in it aren't written again. The writer
// never calls the logger (its lines are logged at the next Present). DXMT_PSO_RECORD=0 records
// nothing and opens nothing. See docs/gw2/README.md.

#include "sha1/sha1_util.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dxmt::compile {
struct PipelineRecipe; // dxmt_compile_recipe.hpp (C3)
}

namespace dxmt::gw2 {

// DXMT_PSO_RECORD (on unless "0"), parsed once.
bool recipeRecording();

// The schema of the records (the database file's name, its table, and every V and R row's header).
constexpr uint32_t kRecipeSchema = 1;

// Explicit little-endian serialization for the records' values.
struct RecipeBytes {
  std::vector<uint8_t> data;
  void
  u8(uint8_t v) {
    data.push_back(v);
  }
  void
  u32(uint32_t v) {
    for (int i = 0; i < 4; i++)
      data.push_back(uint8_t(v >> (8 * i)));
  }
  void
  digest(const Sha1Digest &d) {
    data.insert(data.end(), d.data, d.data + sizeof(d.data));
  }
};

enum class RecipeVariantKind : uint8_t { Vertex = 1, Pixel = 2, Plain = 3 };

// CreateShader, for a shader new in this process: the bytecode is copied.
void recipeShader(const Sha1Digest &sha1, const void *bytecode, size_t length);
// CreateVariantShader: the variant's translation inputs, serialized by the caller. `metal_version`
// is the translation target (SM50_SHADER_METAL_VERSION, the device's metalVersion()); it is part
// of the variant's recorded identity. False if the variant wasn't admitted (then pipelines using it
// are counted unsupported instead of referring to a missing row).
bool recipeVariant(const Sha1Digest &shader, const Sha1Digest &variant, uint32_t metal_version,
                   RecipeVariantKind kind, RecipeBytes &&inputs);
void recipeVariantUnsupported();
// A plain graphics pipeline's build (the compile worker, right before Metal builds it): its recipe
// (C3), serialized by the one canonical serializer. The recipe must carry the vertex identity.
void recipePipeline(const compile::PipelineRecipe &recipe);
void recipePipelineUnsupported();
// gw2 (#124): a pipeline taken from warming's shared results instead of built: notes the recipe's use
// for this session (its U row, DXMT_PSO_RECORD_USE), as recipePipeline does for a build. `recipe_key`
// is the recipe's SHA-1 (compile::recipeKey with the variants' identities). Installed as warming's
// hit hook (setWarmHitUse); warmPipelineHit calls it.
void recipeUse(const Sha1Digest &recipe_key);
// gw2 (#124): DXMT_PSO_RECORD_USE (on unless "0") with DXMT_PSO_RECORD on, parsed once.
bool recipeUseRecording();

// The writer thread's log lines, logged by the render thread at Present (try_lock only).
void recipeLogPending();

} // namespace dxmt::gw2
