#pragma once

// gw2 (#57): shader warming, stage W1. Records, durably and per session, what each pipeline was
// built from, independent of D3D11 objects, so later sessions can prepare shaders and pipelines
// before GW2 asks for them (W2/W3):
//
// - the DXBC of every shader, by its SHA-1 (the AIR can be regenerated after an airconv change);
// - each shader variant's translation inputs, by (shader SHA-1, variant digest);
// - each graphics pipeline's recipe: the vertex and pixel shader identities (SHA-1 and variant
//   digest) and every field of the WMTRenderPipelineInfo handed to Metal, serialized explicitly
//   (never pointers or padding), keyed by the SHA-1 of that serialization.
//
// Into $(getconf DARWIN_USER_CACHE_DIR)dxmt/<exe>/recipes_<schema>.db through the bridge's cache
// writer, from one writer thread fed by a bounded queue: the callers only copy and enqueue, once per
// key and session. DXMT_PSO_RECORD=0 records nothing and opens nothing. See docs/gw2/README.md.

#include "sha1/sha1_util.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

struct WMTRenderPipelineInfo;

namespace dxmt::gw2 {

// DXMT_PSO_RECORD (on unless "0"), parsed once.
bool recipeRecording();

// The schema of the records (the database file's name and every recipe's header).
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
// CreateVariantShader: the variant's translation inputs, serialized by the caller.
void recipeVariant(const Sha1Digest &shader, const Sha1Digest &variant, RecipeVariantKind kind,
                   RecipeBytes &&inputs);
void recipeVariantUnsupported();
// A plain graphics pipeline's build (the compile worker, right before Metal builds it). `ps_*`
// null for a pipeline without a pixel shader.
void recipePipeline(const Sha1Digest &vs_shader, const Sha1Digest &vs_variant, const Sha1Digest *ps_shader,
                    const Sha1Digest *ps_variant, const WMTRenderPipelineInfo &info);
void recipePipelineUnsupported();

} // namespace dxmt::gw2
