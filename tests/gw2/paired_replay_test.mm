// gw2-test: metal
// gw2-build: U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
// gw2-build: (( ${#U} >= 5 ))
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   for f in $U; do clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Wno-unused-function -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $T/u_${f:r}_$arch.o; done
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal src/winemetal/winemetal_thunks.c -o $T/thunks_$arch.o
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c -o $T/airconv_absent_$arch.o
// gw2-build:   clang++ -std=c++20 -fobjc-arc -Wall -Wextra -arch $arch -mmacosx-version-min=26.0 -Itests/gw2/shim -Isrc/winemetal -Isrc/dxmt tests/gw2/paired_replay_test.mm src/dxmt/dxmt_gw2_mtl4_queue.cpp src/dxmt/dxmt_gw2_mtl4_encoding.cpp $T/thunks_$arch.o $T/u_*_$arch.o $T/airconv_absent_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -o ${OUT}_$arch
// gw2-build: done
// gw2-run: ${OUT}_x86_64 && MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ${OUT}_arm64
//
// gw2 (B4, jdrly/dxmt #105, architecture council 2026-11-04 §5): paired replay fixtures. One frame's
// command packets (wmtcmd_*, as DXMT's encode thread builds them) go through both interpreters of the
// bridge's unix side, and what they drew is compared:
//
// - Metal 3: winemetal_unix.c's _MTL{Render,Compute,Blit}CommandEncoder_encodeCommands, on an
//   MTLCommandQueue's command buffers, as flushCommands encodes into a WMT::CommandBuffer;
// - Metal 4: mtl4.c's replays, through DXMT's own gw2::Mtl4Queue and gw2::Mtl4Encoding (the chunk slot's
//   residency set, the queue's argument tables, the bytes arena with its refill), as flushCommands
//   encodes into a gw2::Mtl4Encoding.
//
// Both go through the real bridge: the PE side (winemetal_thunks.c, Metal.hpp) and the whole unix side
// linked natively with DXMT_NATIVE, each call reaching __wine_unix_call_funcs by its manifest number
// (as mtl4_queue_test and chunk_submission_test). Only airconv is left out (no shader is converted:
// the fixture's shaders are MSL). No Wine, no game. The test adds no seam to production code.
//
// The frame (one builder, called once per backend; the packet-type sequence is checked to be the same):
// 0. A load chunk, committed, completed and retired before the frame (GW2 loads textures in earlier
//    chunks): CopyFromBufferToTexture into an 8x8 texture and a mipmapped 16x16 one. In the frame the
//    8x8 texture is reached only through an argument buffer, so on Metal 4 only its UseResource puts
//    it into the frame chunk's residency set.
// 1. Upload blit (UpdateSubresource): CopyFromBufferToBuffer into a private vertex and index buffer,
//    FillBuffer of the compute output; UpdateFence.
// 2. Scene pass (64x64 RGBA8 + Depth32Float): WaitForFence; UseResource of what only argument buffers
//    reach (the vertex buffer, the texture); SetPSO, SetDSSO, SetRasterizerState, SetViewport,
//    SetScissorRect; argument buffers at index 16 (a buffer's GPU address for the vertex stage, a
//    texture's gpuResourceID for the fragment stage); Map'd ring constants at index 1, moved per draw
//    by Set{Vertex,Fragment}BufferOffset; SetFragmentBytes at index 2; DrawIndexed at an index-buffer
//    offset, Draw, a depth-rejected Draw, and an instanced Draw with a base instance on a blending
//    pipeline with SetBlendFactorAndStencilRef; UpdateFence.
// 3. Rows pass (64x64): 64 draws, each with SetScissorRect on its row and a 4 KB SetFragmentBytes:
//    256 KB of bytes in one encoder, so Metal 4's bytes arena stops the replay and resumes it with a
//    new one (Mtl4Encoding::refill); UpdateFence.
// 4. Compute: WaitForFence, SetPSO, SetTexture (the scene), SetBuffer, SetBytes, Dispatch,
//    MemoryBarrier, a second pipeline (another threadgroup size) and Dispatch; UpdateFence.
// 5. Blit: WaitForFence, CopyFromTextureToTexture (the scene into the "back buffer"), GenerateMipmaps,
//    CopyFromTextureToBuffer readbacks (scene, rows, mip 2); UpdateFence.
// 6. The present split: the frame is committed before nextDrawable; the present pass (its own command
//    buffer; Metal 4: waitForDrawable, WMT_MTL4_NO_ATTACHMENT_RESIDENCY, signalDrawable, present)
//    draws the back buffer into the drawable with SetFragmentTexture and SetFragmentBytes (a gain), as
//    the presenter does, and a blit reads the drawable back before it is presented. A layer without a
//    window gives a drawable here; if it gives none, step 6 is skipped, the rest is still compared, and
//    the test exits 77 (skipped) unless something failed.
//
// What it checks:
// - every readback (scene, rows, mip 2, compute output, drawable) is byte-identical between Metal 3 and
//   Metal 4;
// - each also matches what the CPU computes at sample points (both backends), so a stream that draws
//   nothing, or the wrong thing, on both doesn't pass;
// - no command buffer reports an error; Metal 4: two commits (frame, present), no unsupported packet,
//   no dropped UseResource, no Set*Bytes overflow, the bytes arena refilled at least once inside the rows
//   pass's replay, and the replays put allocations into the chunk's residency set.
//
// How much it sees (measured 2026-11-04 on an M5 Max by mutating a scratch copy of mtl4.c, arm64): each
// of these made the run fail: Set*BufferOffset ignoring its offset, a Set*Bytes overflow skipped instead
// of resumed, DrawIndexed without its index-buffer offset, an instanced draw without its base instance,
// a dropped blend colour, a compute pipeline's threadgroup size ignored, GenerateMipmaps dropped, a
// render pass's WaitForFence dropped. Two need the arm64 run's validation: a dropped UseResource (a
// non-resident texture reads as zero only under MTL_SHADER_VALIDATION=1; without it, the texture stays
// readable), and a blit's dropped WaitForFence (a race; it showed under the validation layer's timing,
// not without it). A missing fence or residency can therefore pass when the race or the fault doesn't
// show.
//
// A known Metal 3/Metal 4 divergence is listed in kKnown (by readback: scene, rows, mip 2, compute,
// drawable) with its fork issue: its Metal 3/Metal 4 comparison and Metal 4's CPU checks of that
// readback are reported as XFAIL and don't fail the run (Metal 3's CPU checks, the control, still do);
// if the comparison starts to match, XPASS fails the run so the entry is removed.
//
// Exit 77 (skipped): no Metal device, or no Metal 4 (macOS 26+, MTLGPUFamilyMetal4).

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "dxmt_gw2_mtl4_encoding.hpp"
#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_stats.hpp"
extern "C" {
#include "winemetal_thunks.h"
}

using dxmt::gw2::Mtl4Encoding;
using dxmt::gw2::Mtl4Queue;

static int failures = 0, xfailed = 0;
// gw2 (#107): set while a known divergence's Metal 4 CPU checks run (KnownScope): their failures are
// expected (XFAIL, with the issue), not counted as failures
static const char *g_known_issue = nullptr;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("%s %s:%d: %s: ", g_known_issue ? "XFAIL" : "FAIL", __FILE__, __LINE__, #cond);  \
      std::printf(__VA_ARGS__);                                                                    \
      if (g_known_issue)                                                                           \
        std::printf(" (known: %s)", g_known_issue);                                                \
      std::printf("\n");                                                                           \
      if (g_known_issue)                                                                           \
        xfailed++;                                                                                 \
      else                                                                                         \
        failures++;                                                                                \
    }                                                                                              \
  } while (0)

// --- what DXMT's statistics module provides
namespace dxmt::gw2 {
void
registerSection(const char *, SectionFn) {}
bool
statsEnabled() {
  return true;
}
// the device set (#29) isn't in this test: the queue's own per-slot sets are
void
residencyBeforeCommit() {}
// gw2 (#153): DXMT_MTL4_CHUNK_RESIDENCY unset
bool
mtl4ChunkResidency() {
  return false;
}
} // namespace dxmt::gw2

template <typename T>
static obj_handle_t
H(T object) {
  return (obj_handle_t)(__bridge void *)object;
}

// --- known divergences (xfail): a comparison's name and the fork issue that tracks it
struct Known {
  const char *what;
  const char *issue;
};
static const Known kKnown[] = {
    {nullptr, nullptr}, // none known
};

static const char *
known(const std::string &what) {
  for (auto &k : kKnown)
    if (k.what && what == k.what)
      return k.issue;
  return nullptr;
}

// gw2 (#107): the CPU checks of one readback, as known(what) says: on Metal 4 (the backend under test)
// a known divergence's check failures are XFAIL; Metal 3's (the control) always count
class KnownScope {
public:
  KnownScope(const char *backend, const char *what) {
    if (std::strcmp(backend, "Metal 4") == 0)
      g_known_issue = known(what);
  }
  ~KnownScope() { g_known_issue = nullptr; }
  KnownScope(const KnownScope &) = delete;
  KnownScope &operator=(const KnownScope &) = delete;
};

// --- a packet chain in one block of memory, as DXMT's encode thread builds an encoder's commands
class Chain {
public:
  Chain() : data_(new char[kSize]) {}

  template <typename T>
  T &
  add(uint16_t type) {
    auto *cmd = reinterpret_cast<T *>(alloc(sizeof(T)));
    auto *base = reinterpret_cast<wmtcmd_base *>(cmd);
    base->type = type;
    base->next.set(nullptr);
    if (last_)
      last_->next.set(base);
    else
      head_ = base;
    last_ = base;
    types_.push_back(type);
    return *cmd;
  }

  // a Set*Bytes payload, kept with the chain (the encode thread's memory)
  void *
  bytes(const void *src, size_t size) {
    void *dst = alloc(size);
    std::memcpy(dst, src, size);
    return dst;
  }

  template <typename T>
  const T *
  head() const {
    return reinterpret_cast<const T *>(head_);
  }

  const std::vector<uint16_t> &
  types() const {
    return types_;
  }

private:
  static constexpr size_t kSize = 512 * 1024;

  void *
  alloc(size_t size) {
    size = (size + 15) & ~size_t(15);
    if (used_ + size > kSize)
      std::abort();
    void *p = data_.get() + used_;
    std::memset(p, 0, size);
    used_ += size;
    return p;
  }

  std::unique_ptr<char[]> data_;
  size_t used_ = 0;
  wmtcmd_base *head_ = nullptr, *last_ = nullptr;
  std::vector<uint16_t> types_;
};

// --- the fixture's shaders and pipelines (built once, natively: setup, not under test)
static const char *kSource = R"(
#include <metal_stdlib>
using namespace metal;
struct Vtx { float2 pos; float2 uv; };
struct VArgs { device const Vtx *vb; };        // argument buffer (vertex): the vertex buffer's address
struct FArgs { texture2d<float> tex; };        // argument buffer (fragment): the texture's resource ID
struct DrawCB { float4 color; float4 xform; float4 z; }; // Map'd ring constants: offset xy, scale zw
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vs_scene(uint vid [[vertex_id]], uint iid [[instance_id]],
                     constant VArgs &va [[buffer(16)]], constant DrawCB &cb [[buffer(1)]]) {
  Vtx v = va.vb[vid];
  VOut o;
  float2 p = v.pos * cb.xform.zw + cb.xform.xy + float2(0.25 * float(iid), 0);
  o.pos = float4(p, cb.z.x, 1);
  o.uv = v.uv;
  return o;
}
fragment float4 fs_scene(VOut in [[stage_in]], constant FArgs &fa [[buffer(16)]],
                         constant DrawCB &cb [[buffer(1)]], constant float4 &add [[buffer(2)]]) {
  uint2 t = uint2(clamp(in.uv, 0.0, 0.999) * 8.0);
  return fa.tex.read(t) * cb.color + add;
}
struct FullOut { float4 pos [[position]]; };
vertex FullOut vs_full(uint vid [[vertex_id]]) {
  FullOut o;
  o.pos = float4(vid == 1 ? 3.0 : -1.0, vid == 2 ? 3.0 : -1.0, 0, 1);
  return o;
}
struct Rows { float4 color; uint4 fill[255]; }; // 4 KB, all of it read
fragment float4 fs_rows(constant Rows &r [[buffer(0)]]) {
  return float4(r.color.rgb, float(r.fill[254].w & 0xff) / 255.0);
}
fragment float4 fs_present(FullOut in [[stage_in]], texture2d<float> back [[texture(0)]],
                           constant float4 &gain [[buffer(0)]]) {
  return back.read(uint2(in.pos.xy)) * gain;
}
struct CsParams { uint xor_mask; uint width; };
kernel void cs_pack(texture2d<float, access::read> scene [[texture(0)]], device uint *out [[buffer(0)]],
                    constant CsParams &p [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {
  out[gid.y * p.width + gid.x] = pack_float_to_unorm4x8(scene.read(gid)) ^ p.xor_mask;
}
kernel void cs_add(device uint *out [[buffer(0)]], constant uint &add [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {
  out[i] += add;
}
)";

constexpr uint32_t kSize = 64;      // the render targets
constexpr uint32_t kTex = 8;        // the scene's texture
constexpr uint32_t kMip = 16;       // the mipmapped texture's level 0
constexpr uint32_t kOutWords = kSize * kSize;
constexpr uint32_t kOutTail = 256;  // bytes after the compute output that only FillBuffer writes
constexpr uint32_t kXor = 0x5a5a5a5au, kAdd = 3;
constexpr float kGain[4] = {0.5f, 1.0f, 1.0f, 1.0f};

struct Fixture {
  id<MTLDevice> device;
  id<MTLRenderPipelineState> scene, blend, rows, present;
  id<MTLComputePipelineState> pack, add;
  id<MTLDepthStencilState> less_write, always;
};

static bool
makeFixture(Fixture &fx) {
  NSError *err = nil;
  id<MTLLibrary> lib = [fx.device newLibraryWithSource:@(kSource) options:nil error:&err];
  CHECK(lib, "shader source: %s", err.description.UTF8String);
  if (!lib)
    return false;
  auto pipeline = [&](NSString *vs, NSString *fs, MTLPixelFormat color, bool depth, bool blend) {
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:vs];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = color;
    if (depth)
      d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if (blend) {
      d.colorAttachments[0].blendingEnabled = YES;
      d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorBlendColor;
      d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusBlendColor;
      d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
      d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
    }
    NSError *e = nil;
    id<MTLRenderPipelineState> pso = [fx.device newRenderPipelineStateWithDescriptor:d error:&e];
    CHECK(pso, "render pipeline %s/%s: %s", vs.UTF8String, fs.UTF8String, e.description.UTF8String);
    return pso;
  };
  fx.scene = pipeline(@"vs_scene", @"fs_scene", MTLPixelFormatRGBA8Unorm, true, false);
  fx.blend = pipeline(@"vs_scene", @"fs_scene", MTLPixelFormatRGBA8Unorm, true, true);
  fx.rows = pipeline(@"vs_full", @"fs_rows", MTLPixelFormatRGBA8Unorm, false, false);
  fx.present = pipeline(@"vs_full", @"fs_present", MTLPixelFormatBGRA8Unorm, false, false);
  fx.pack = [fx.device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_pack"] error:&err];
  fx.add = [fx.device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_add"] error:&err];
  CHECK(fx.pack && fx.add, "compute pipelines: %s", err.description.UTF8String);
  MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
  dd.depthCompareFunction = MTLCompareFunctionLess;
  dd.depthWriteEnabled = YES;
  fx.less_write = [fx.device newDepthStencilStateWithDescriptor:dd];
  dd.depthCompareFunction = MTLCompareFunctionAlways;
  dd.depthWriteEnabled = NO;
  fx.always = [fx.device newDepthStencilStateWithDescriptor:dd];
  return fx.scene && fx.blend && fx.rows && fx.present && fx.pack && fx.add && fx.less_write && fx.always;
}

// --- one run's resources (each backend gets its own, with the same contents)
struct Vtx {
  float pos[2], uv[2];
};
struct DrawCB {
  float color[4], xform[4], z[4];
};
struct CsParams {
  uint32_t xor_mask, width;
};
struct Rows {
  float color[4];
  uint32_t fill[255][4];
};
static_assert(sizeof(Rows) == 4096);

// the ring's slices (Map WRITE_DISCARD, one per draw)
enum { kDrawA, kDrawB, kDrawD, kDrawC, kDraws };

// staging layout (UpdateSubresource's sources)
constexpr uint64_t kStageTex = 0, kStageMip = 1024, kStageVb = 2048, kStageIb = 3072, kStageSize = 4096;

static const Vtx kVertices[] = {
    // 0-3: a quad over the left half, uv 0..1 with (0,0) at the top left
    {{-1, 1}, {0, 0}},
    {{0, 1}, {1, 0}},
    {{-1, -1}, {0, 1}},
    {{0, -1}, {1, 1}},
    // 4-6: a triangle over the whole target, uv on texel (0,0) (red)
    {{-1, -1}, {0, 0}},
    {{3, -1}, {0, 0}},
    {{-1, 3}, {0, 0}},
    // 7-9: the same, uv on texel (7,7) (white)
    {{-1, -1}, {0.95f, 0.95f}},
    {{3, -1}, {0.95f, 0.95f}},
    {{-1, 3}, {0.95f, 0.95f}},
    // 10-15: two triangles, an 8x8 px square around (0,0), uv on the white texel
    {{-0.125f, 0.125f}, {0.95f, 0.95f}},
    {{0.125f, 0.125f}, {0.95f, 0.95f}},
    {{-0.125f, -0.125f}, {0.95f, 0.95f}},
    {{-0.125f, -0.125f}, {0.95f, 0.95f}},
    {{0.125f, 0.125f}, {0.95f, 0.95f}},
    {{0.125f, -0.125f}, {0.95f, 0.95f}},
};
// drawn from byte 4: the first two are junk the offset must skip
static const uint16_t kIndices[] = {9, 9, 0, 1, 2, 2, 1, 3};

// the texture's quadrants (texel x < 4 / >= 4, y < 4 / >= 4)
static const uint8_t kQuadrant[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};

struct Resources {
  id<MTLBuffer> staging, vb, ib, ring, argbuf, out, rb_scene, rb_rows, rb_mip, rb_present;
  id<MTLTexture> tex, mip, scene, depth, rows, back;
  id<MTLFence> upload, scene_done, rows_done, compute_done, blit_done, present_done;
};

static id<MTLBuffer>
newBuffer(id<MTLDevice> device, NSUInteger length, MTLResourceOptions storage) {
  id<MTLBuffer> b = [device newBufferWithLength:length options:storage | MTLResourceHazardTrackingModeUntracked];
  if (storage == MTLResourceStorageModeShared)
    std::memset(b.contents, 0xcd, length);
  return b;
}

static id<MTLTexture>
newTexture(id<MTLDevice> device, MTLPixelFormat format, uint32_t size, bool mipmapped, MTLTextureUsage usage) {
  MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                               width:size
                                                                              height:size
                                                                           mipmapped:mipmapped];
  d.usage = usage;
  d.storageMode = MTLStorageModePrivate;
  d.hazardTrackingMode = MTLHazardTrackingModeUntracked;
  return [device newTextureWithDescriptor:d];
}

static Resources
makeResources(id<MTLDevice> device) {
  Resources r;
  auto shared = MTLResourceStorageModeShared, priv = MTLResourceStorageModePrivate;
  r.staging = newBuffer(device, kStageSize, shared);
  r.vb = newBuffer(device, sizeof(kVertices), priv);
  r.ib = newBuffer(device, sizeof(kIndices), priv);
  r.ring = newBuffer(device, 256 * kDraws, shared);
  r.argbuf = newBuffer(device, 512, shared);
  r.out = newBuffer(device, kOutWords * 4 + kOutTail, shared);
  r.rb_scene = newBuffer(device, kSize * kSize * 4, shared);
  r.rb_rows = newBuffer(device, kSize * kSize * 4, shared);
  r.rb_mip = newBuffer(device, (kMip / 4) * (kMip / 4) * 4, shared);
  r.rb_present = newBuffer(device, kSize * kSize * 4, shared);
  r.tex = newTexture(device, MTLPixelFormatRGBA8Unorm, kTex, false, MTLTextureUsageShaderRead);
  r.mip = newTexture(device, MTLPixelFormatRGBA8Unorm, kMip, true, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
  r.scene = newTexture(device, MTLPixelFormatRGBA8Unorm, kSize, false, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
  r.depth = newTexture(device, MTLPixelFormatDepth32Float, kSize, false, MTLTextureUsageRenderTarget);
  r.rows = newTexture(device, MTLPixelFormatRGBA8Unorm, kSize, false, MTLTextureUsageRenderTarget);
  r.back = newTexture(device, MTLPixelFormatRGBA8Unorm, kSize, false, MTLTextureUsageShaderRead);
  r.upload = [device newFence];
  r.scene_done = [device newFence];
  r.rows_done = [device newFence];
  r.compute_done = [device newFence];
  r.blit_done = [device newFence];
  r.present_done = [device newFence];

  // staging: the texture (8x8, quadrants), the mipmapped texture's level 0 (16x16, quadrants), the
  // vertices, the indices
  auto *stage = static_cast<uint8_t *>(r.staging.contents);
  for (uint32_t y = 0; y < kTex; y++)
    for (uint32_t x = 0; x < kTex; x++)
      std::memcpy(stage + kStageTex + (y * kTex + x) * 4, kQuadrant[(y >= 4) * 2 + (x >= 4)], 4);
  for (uint32_t y = 0; y < kMip; y++)
    for (uint32_t x = 0; x < kMip; x++)
      std::memcpy(stage + kStageMip + (y * kMip + x) * 4, kQuadrant[(y >= 8) * 2 + (x >= 8)], 4);
  std::memcpy(stage + kStageVb, kVertices, sizeof(kVertices));
  std::memcpy(stage + kStageIb, kIndices, sizeof(kIndices));

  // the ring (Map WRITE_DISCARD): one slice per draw
  auto slice = [&](int i) { return reinterpret_cast<DrawCB *>(static_cast<uint8_t *>(r.ring.contents) + 256 * i); };
  // A: the textured quad, at z 0.5, as is
  *slice(kDrawA) = DrawCB{{1, 1, 1, 1}, {0, 0, 1, 1}, {0.5f, 0, 0, 0}};
  // B: the red texel over the top-right quadrant, scaled, at z 0.25
  *slice(kDrawB) = DrawCB{{0.6f, 1, 1, 1}, {0.5f, 0.5f, 0.5f, 0.5f}, {0.25f, 0, 0, 0}};
  // D: white texel times dark green over everything at z 0.75: only the bottom-right quadrant passes
  *slice(kDrawD) = DrawCB{{0, 0.4f, 0, 1}, {0, 0, 1, 1}, {0.75f, 0, 0, 0}};
  // C: blue, two 8x8 squares around (0.5,-0.5) and (0.75,-0.5) (instances 1 and 2), blended
  *slice(kDrawC) = DrawCB{{0, 0, 1, 1}, {0.25f, -0.5f, 1, 1}, {0.1f, 0, 0, 0}};

  // the argument buffer, as DXMT writes it: the vertex buffer's address at 0 (vertex), the texture's
  // resource ID at 256 (fragment)
  uint64_t vb_address = r.vb.gpuAddress;
  std::memcpy(r.argbuf.contents, &vb_address, 8);
  MTLResourceID tex_id = r.tex.gpuResourceID;
  std::memcpy(static_cast<uint8_t *>(r.argbuf.contents) + 256, &tex_id, 8);
  return r;
}

// --- the frame's packet streams (the same builders for both backends)
struct Stream {
  Chain load, upload, scene, rows, compute, blit, present_pass, present_blit;
  std::vector<std::vector<uint16_t>>
  types() const {
    return {load.types(), upload.types(), scene.types(), rows.types(), compute.types(), blit.types(), present_pass.types(),
            present_blit.types()};
  }
};

static void
buildFrame(Stream &s, const Fixture &fx, const Resources &r) {
  // 0. the load chunk, committed and retired before the frame (GW2 loads textures in earlier chunks):
  //    in the frame, the scene's texture is then reached only through the argument buffer, and only
  //    its UseResource makes it resident on Metal 4
  {
    Chain &c = s.load;
    auto &tex = c.add<wmtcmd_blit_copy_from_buffer_to_texture>(WMTBlitCommandCopyFromBufferToTexture);
    tex.src = H(r.staging);
    tex.src_offset = kStageTex;
    tex.bytes_per_row = kTex * 4;
    tex.bytes_per_image = kTex * kTex * 4;
    tex.size = {kTex, kTex, 1};
    tex.dst = H(r.tex);
    auto &mip = c.add<wmtcmd_blit_copy_from_buffer_to_texture>(WMTBlitCommandCopyFromBufferToTexture);
    mip.src = H(r.staging);
    mip.src_offset = kStageMip;
    mip.bytes_per_row = kMip * 4;
    mip.bytes_per_image = kMip * kMip * 4;
    mip.size = {kMip, kMip, 1};
    mip.dst = H(r.mip);
  }
  // 1. upload (UpdateSubresource)
  {
    Chain &c = s.upload;
    auto &vb = c.add<wmtcmd_blit_copy_from_buffer_to_buffer>(WMTBlitCommandCopyFromBufferToBuffer);
    vb.src = H(r.staging);
    vb.src_offset = kStageVb;
    vb.dst = H(r.vb);
    vb.copy_length = sizeof(kVertices);
    auto &ib = c.add<wmtcmd_blit_copy_from_buffer_to_buffer>(WMTBlitCommandCopyFromBufferToBuffer);
    ib.src = H(r.staging);
    ib.src_offset = kStageIb;
    ib.dst = H(r.ib);
    ib.copy_length = sizeof(kIndices);
    auto &fill = c.add<wmtcmd_blit_fillbuffer>(WMTBlitCommandFillBuffer);
    fill.buffer = H(r.out);
    fill.length = kOutWords * 4 + kOutTail;
    fill.value = 0xab;
    c.add<wmtcmd_blit_fence_op>(WMTBlitCommandUpdateFence).fence = H(r.upload);
  }
  // 2. the scene pass
  {
    Chain &c = s.scene;
    auto &wait = c.add<wmtcmd_render_fence_op>(WMTRenderCommandWaitForFence);
    wait.fence = H(r.upload);
    wait.stages = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
    auto use = [&](id<MTLResource> res, WMTRenderStages stages) {
      auto &u = c.add<wmtcmd_render_useresource>(WMTRenderCommandUseResource);
      u.resource = H(res);
      u.usage = WMTResourceUsageRead;
      u.stages = stages;
    };
    use(r.vb, WMTRenderStageVertex);
    use(r.tex, WMTRenderStageFragment);
    c.add<wmtcmd_render_setpso>(WMTRenderCommandSetPSO).pso = H(fx.scene);
    auto &dsso = c.add<wmtcmd_render_setdsso>(WMTRenderCommandSetDSSO);
    dsso.dsso = H(fx.less_write);
    auto &rs = c.add<wmtcmd_render_setrasterizerstate>(WMTRenderCommandSetRasterizerState);
    rs.fill_mode = WMTTriangleFillModeFill;
    rs.cull_mode = WMTCullModeNone;
    rs.depth_clip_mode = WMTDepthClipModeClip;
    rs.winding = WMTWindingClockwise;
    c.add<wmtcmd_render_setviewport>(WMTRenderCommandSetViewport).viewport = {0, 0, kSize, kSize, 0, 1};
    c.add<wmtcmd_render_setscissorrect>(WMTRenderCommandSetScissorRect).scissor_rect = {0, 0, kSize, kSize};
    auto setBuffer = [&](uint16_t type, id<MTLBuffer> buffer, uint64_t offset, uint8_t index) {
      auto &b = c.add<wmtcmd_render_setbuffer>(type);
      b.buffer = H(buffer);
      b.offset = offset;
      b.index = index;
    };
    auto setOffset = [&](uint16_t type, uint64_t offset, uint8_t index) {
      auto &b = c.add<wmtcmd_render_setbufferoffset>(type);
      b.offset = offset;
      b.index = index;
    };
    auto ringAt = [&](int draw) {
      setOffset(WMTRenderCommandSetVertexBufferOffset, 256 * draw, 1);
      setOffset(WMTRenderCommandSetFragmentBufferOffset, 256 * draw, 1);
    };
    auto bytes = [&](float r0, float g0, float b0, float a0) {
      float add[4] = {r0, g0, b0, a0};
      auto &b = c.add<wmtcmd_render_setbytes>(WMTRenderCommandSetFragmentBytes);
      b.bytes.set(c.bytes(add, sizeof(add)));
      b.length = sizeof(add);
      b.index = 2;
    };
    setBuffer(WMTRenderCommandSetVertexBuffer, r.argbuf, 0, 16);
    setBuffer(WMTRenderCommandSetFragmentBuffer, r.argbuf, 256, 16);
    setBuffer(WMTRenderCommandSetVertexBuffer, r.ring, 0, 1);
    setBuffer(WMTRenderCommandSetFragmentBuffer, r.ring, 0, 1);
    bytes(0, 0, 0, 0);
    // A: indexed, from byte 4 of the index buffer
    auto &a = c.add<wmtcmd_render_draw_indexed>(WMTRenderCommandDrawIndexed);
    a.primitive_type = WMTPrimitiveTypeTriangle;
    a.index_type = WMTIndexTypeUInt16;
    a.index_count = 6;
    a.index_buffer = H(r.ib);
    a.index_buffer_offset = 4;
    a.instance_count = 1;
    auto draw = [&](uint64_t start, uint64_t count, uint32_t instances, uint32_t base_instance) {
      auto &d = c.add<wmtcmd_render_draw>(WMTRenderCommandDraw);
      d.primitive_type = WMTPrimitiveTypeTriangle;
      d.vertex_start = start;
      d.vertex_count = count;
      d.instance_count = instances;
      d.base_instance = base_instance;
    };
    // B: in front of nothing, over the top-right quadrant
    ringAt(kDrawB);
    draw(4, 3, 1, 0);
    // D: behind A and B, so only the bottom-right quadrant
    ringAt(kDrawD);
    draw(7, 3, 1, 0);
    // C: blended, instances 1 and 2, with a new add
    c.add<wmtcmd_render_setpso>(WMTRenderCommandSetPSO).pso = H(fx.blend);
    auto &dsso2 = c.add<wmtcmd_render_setdsso>(WMTRenderCommandSetDSSO);
    dsso2.dsso = H(fx.always);
    auto &blend = c.add<wmtcmd_render_setblendcolor>(WMTRenderCommandSetBlendFactorAndStencilRef);
    blend.red = blend.green = blend.blue = blend.alpha = 0.5f;
    ringAt(kDrawC);
    bytes(0.25f, 0, 0, 0);
    draw(10, 6, 2, 1);
    auto &update = c.add<wmtcmd_render_fence_op>(WMTRenderCommandUpdateFence);
    update.fence = H(r.scene_done);
    update.stages = WMTRenderStageFragment;
  }
  // 3. the rows pass: 64 x 4 KB of SetFragmentBytes in one encoder
  {
    Chain &c = s.rows;
    c.add<wmtcmd_render_setpso>(WMTRenderCommandSetPSO).pso = H(fx.rows);
    c.add<wmtcmd_render_setviewport>(WMTRenderCommandSetViewport).viewport = {0, 0, kSize, kSize, 0, 1};
    for (uint32_t i = 0; i < kSize; i++) {
      c.add<wmtcmd_render_setscissorrect>(WMTRenderCommandSetScissorRect).scissor_rect = {0, i, kSize, 1};
      Rows rows;
      rows.color[0] = float(i) / 63.0f;
      rows.color[1] = 1.0f - float(i) / 63.0f;
      rows.color[2] = float(i % 2);
      rows.color[3] = 1;
      for (auto &f : rows.fill)
        f[0] = f[1] = f[2] = f[3] = 0xee00u;
      rows.fill[254][3] = 0xee00u | i; // the last word: only a whole copy carries it
      auto &b = c.add<wmtcmd_render_setbytes>(WMTRenderCommandSetFragmentBytes);
      b.bytes.set(c.bytes(&rows, sizeof(rows)));
      b.length = sizeof(rows);
      b.index = 0;
      auto &d = c.add<wmtcmd_render_draw>(WMTRenderCommandDraw);
      d.primitive_type = WMTPrimitiveTypeTriangle;
      d.vertex_count = 3;
      d.instance_count = 1;
    }
    auto &update = c.add<wmtcmd_render_fence_op>(WMTRenderCommandUpdateFence);
    update.fence = H(r.rows_done);
    update.stages = WMTRenderStageFragment;
  }
  // 4. compute
  {
    Chain &c = s.compute;
    c.add<wmtcmd_compute_fence_op>(WMTComputeCommandWaitForFence).fence = H(r.scene_done);
    auto &pso = c.add<wmtcmd_compute_setpso>(WMTComputeCommandSetPSO);
    pso.pso = H(fx.pack);
    pso.threadgroup_size = {8, 8, 1};
    auto &tex = c.add<wmtcmd_compute_settexture>(WMTComputeCommandSetTexture);
    tex.texture = H(r.scene);
    tex.index = 0;
    auto &buf = c.add<wmtcmd_compute_setbuffer>(WMTComputeCommandSetBuffer);
    buf.buffer = H(r.out);
    buf.index = 0;
    CsParams params{kXor, kSize};
    auto &bytes = c.add<wmtcmd_compute_setbytes>(WMTComputeCommandSetBytes);
    bytes.bytes.set(c.bytes(&params, sizeof(params)));
    bytes.length = sizeof(params);
    bytes.index = 1;
    c.add<wmtcmd_compute_dispatch>(WMTComputeCommandDispatch).size = {kSize / 8, kSize / 8, 1};
    c.add<wmtcmd_compute_memory_barrier>(WMTComputeCommandMemoryBarrier).scope = WMTBarrierScopeBuffers;
    auto &pso2 = c.add<wmtcmd_compute_setpso>(WMTComputeCommandSetPSO);
    pso2.pso = H(fx.add);
    pso2.threadgroup_size = {64, 1, 1};
    uint32_t add = kAdd;
    auto &bytes2 = c.add<wmtcmd_compute_setbytes>(WMTComputeCommandSetBytes);
    bytes2.bytes.set(c.bytes(&add, sizeof(add)));
    bytes2.length = sizeof(add);
    bytes2.index = 1;
    c.add<wmtcmd_compute_dispatch>(WMTComputeCommandDispatch).size = {kOutWords / 64, 1, 1};
    c.add<wmtcmd_compute_fence_op>(WMTComputeCommandUpdateFence).fence = H(r.compute_done);
  }
  // 5. blit: the back buffer, the mips, the readbacks
  {
    Chain &c = s.blit;
    c.add<wmtcmd_blit_fence_op>(WMTBlitCommandWaitForFence).fence = H(r.compute_done);
    c.add<wmtcmd_blit_fence_op>(WMTBlitCommandWaitForFence).fence = H(r.rows_done);
    auto &copy = c.add<wmtcmd_blit_copy_from_texture_to_texture>(WMTBlitCommandCopyFromTextureToTexture);
    copy.src = H(r.scene);
    copy.src_size = {kSize, kSize, 1};
    copy.dst = H(r.back);
    c.add<wmtcmd_blit_generate_mipmaps>(WMTBlitCommandGenerateMipmaps).texture = H(r.mip);
    auto readback = [&](id<MTLTexture> src, uint32_t level, uint32_t size, id<MTLBuffer> dst) {
      auto &rb = c.add<wmtcmd_blit_copy_from_texture_to_buffer>(WMTBlitCommandCopyFromTextureToBuffer);
      rb.src = H(src);
      rb.level = level;
      rb.size = {size, size, 1};
      rb.dst = H(dst);
      rb.bytes_per_row = size * 4;
      rb.bytes_per_image = size * size * 4;
    };
    readback(r.scene, 0, kSize, r.rb_scene);
    readback(r.rows, 0, kSize, r.rb_rows);
    readback(r.mip, 2, kMip / 4, r.rb_mip);
    c.add<wmtcmd_blit_fence_op>(WMTBlitCommandUpdateFence).fence = H(r.blit_done);
  }
}

// the present pass's packets: they name the drawable's texture, known only after nextDrawable
static void
buildPresent(Stream &s, const Fixture &fx, const Resources &r, obj_handle_t drawable_texture) {
  {
    Chain &c = s.present_pass;
    auto &wait = c.add<wmtcmd_render_fence_op>(WMTRenderCommandWaitForFence);
    wait.fence = H(r.blit_done);
    wait.stages = WMTRenderStageFragment;
    auto &tex = c.add<wmtcmd_render_settexture>(WMTRenderCommandSetFragmentTexture);
    tex.texture = H(r.back);
    tex.index = 0;
    auto &bytes = c.add<wmtcmd_render_setbytes>(WMTRenderCommandSetFragmentBytes);
    bytes.bytes.set(c.bytes(kGain, sizeof(kGain)));
    bytes.length = sizeof(kGain);
    bytes.index = 0;
    c.add<wmtcmd_render_setpso>(WMTRenderCommandSetPSO).pso = H(fx.present);
    c.add<wmtcmd_render_setviewport>(WMTRenderCommandSetViewport).viewport = {0, 0, kSize, kSize, 0, 1};
    auto &d = c.add<wmtcmd_render_draw>(WMTRenderCommandDraw);
    d.primitive_type = WMTPrimitiveTypeTriangle;
    d.vertex_count = 3;
    d.instance_count = 1;
    auto &update = c.add<wmtcmd_render_fence_op>(WMTRenderCommandUpdateFence);
    update.fence = H(r.present_done);
    update.stages = WMTRenderStageFragment;
  }
  {
    Chain &c = s.present_blit;
    c.add<wmtcmd_blit_fence_op>(WMTBlitCommandWaitForFence).fence = H(r.present_done);
    auto &rb = c.add<wmtcmd_blit_copy_from_texture_to_buffer>(WMTBlitCommandCopyFromTextureToBuffer);
    rb.src = drawable_texture;
    rb.size = {kSize, kSize, 1};
    rb.dst = H(r.rb_present);
    rb.bytes_per_row = kSize * 4;
    rb.bytes_per_image = kSize * kSize * 4;
  }
}

// --- the two backends, shaped as flushCommandsImpl's two targets

// Metal 3: command buffers from an MTLCommandQueue; the present split commits the frame's and continues
// in a new one (as CommandQueue::CreateCommandBuffer)
struct Metal3Target {
  WMT::CommandQueue queue;
  WMT::CommandBuffer cmdbuf;
  std::vector<WMT::CommandBuffer> committed;

  explicit Metal3Target(WMT::CommandQueue q) : queue(q), cmdbuf(q.commandBuffer()) {}

  WMT::RenderCommandEncoder
  render(WMTRenderPassInfo &info, bool) {
    return cmdbuf.renderCommandEncoder(info);
  }
  WMT::BlitCommandEncoder
  blit() {
    return cmdbuf.blitCommandEncoder();
  }
  WMT::ComputeCommandEncoder
  compute() {
    return cmdbuf.computeCommandEncoder(true);
  }
  void
  split() {
    cmdbuf.commit();
    committed.push_back(cmdbuf);
    cmdbuf = queue.commandBuffer();
  }
  void
  beforePresentPass(WMT::MetalDrawable) {}
  void
  present(WMT::MetalDrawable drawable) {
    cmdbuf.presentDrawable(drawable);
  }
  void
  end() {
    cmdbuf.commit();
    committed.push_back(cmdbuf);
  }
};

// Metal 4: DXMT's own encoding over gw2::Mtl4Queue
struct Metal4Target {
  Mtl4Encoding &encoding;
  bool in_rows = false; // the rows pass's replay is running (its arena refills are the resumes)
  uint32_t rows_refills = 0;

  WMT::MTL4RenderCommandEncoder
  render(WMTRenderPassInfo &info, bool present) {
    return encoding.renderCommandEncoder(info, present ? WMT_MTL4_NO_ATTACHMENT_RESIDENCY : 0);
  }
  WMT::MTL4BlitCommandEncoder
  blit() {
    return encoding.blitCommandEncoder();
  }
  WMT::MTL4ComputeCommandEncoder
  compute() {
    return encoding.computeCommandEncoder(true);
  }
  void
  split() {
    encoding.commitOpen();
  }
  void
  beforePresentPass(WMT::MetalDrawable drawable) {
    encoding.waitForDrawable(drawable);
  }
  void
  present(WMT::MetalDrawable drawable) {
    encoding.signalDrawable(drawable); // commits the present pass first
    drawable.present();
  }
  void
  end() {
    encoding.commitOpen();
  }
};

static void
colorPass(WMTRenderPassInfo &info, obj_handle_t texture, WMTLoadAction load) {
  WMT::InitializeRenderPassInfo(info);
  info.colors[0].texture = texture;
  info.colors[0].load_action = load;
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].clear_color = {0, 0, 0.5, 1};
  info.default_raster_sample_count = 1;
  info.render_target_width = kSize;
  info.render_target_height = kSize;
}

struct RunResult {
  std::vector<std::vector<uint16_t>> types;
  bool presented = false;
};

// the frame through one backend: flushCommands' order, the present split, the present
// the load chunk through one backend (the caller waits for it and retires it)
template <typename Target>
static void
encodeLoad(Target &target, const Stream &s) {
  auto enc = target.blit();
  enc.encodeCommands(s.load.head<wmtcmd_blit_nop>());
  enc.endEncoding();
  target.end();
}

template <typename Target>
static RunResult
encodeFrame(Target &target, Stream &s, const Fixture &fx, const Resources &r, WMT::MetalLayer layer) {
  RunResult result;
  {
    auto enc = target.blit();
    enc.encodeCommands(s.upload.head<wmtcmd_blit_nop>());
    enc.endEncoding();
  }
  {
    WMTRenderPassInfo info;
    colorPass(info, H(r.scene), WMTLoadActionClear);
    info.depth.texture = H(r.depth);
    info.depth.load_action = WMTLoadActionClear;
    info.depth.store_action = WMTStoreActionDontCare;
    info.depth.clear_depth = 1.0f;
    auto enc = target.render(info, false);
    enc.encodeCommands(s.scene.head<wmtcmd_render_nop>());
    enc.endEncoding();
  }
  {
    WMTRenderPassInfo info;
    colorPass(info, H(r.rows), WMTLoadActionClear);
    auto enc = target.render(info, false);
    if constexpr (std::is_same_v<Target, Metal4Target>)
      target.in_rows = true;
    enc.encodeCommands(s.rows.head<wmtcmd_render_nop>());
    if constexpr (std::is_same_v<Target, Metal4Target>)
      target.in_rows = false;
    enc.endEncoding();
  }
  {
    auto enc = target.compute();
    enc.encodeCommands(s.compute.head<wmtcmd_compute_nop>());
    enc.endEncoding();
  }
  {
    auto enc = target.blit();
    enc.encodeCommands(s.blit.head<wmtcmd_blit_nop>());
    enc.endEncoding();
  }
  // the present split: the frame's work is committed before nextDrawable
  target.split();
  WMT::MetalDrawable drawable = layer.nextDrawable();
  if (drawable) {
    WMT::Texture texture = drawable.texture();
    buildPresent(s, fx, r, texture.handle);
    target.beforePresentPass(drawable);
    WMTRenderPassInfo info;
    colorPass(info, texture.handle, WMTLoadActionDontCare);
    {
      auto enc = target.render(info, true);
      enc.encodeCommands(s.present_pass.head<wmtcmd_render_nop>());
      enc.endEncoding();
    }
    {
      auto enc = target.blit();
      enc.encodeCommands(s.present_blit.head<wmtcmd_blit_nop>());
      enc.endEncoding();
    }
    target.present(drawable);
    result.presented = true;
  }
  target.end();
  result.types = s.types();
  return result;
}

// --- comparisons
static bool
near(uint8_t a, uint8_t b) {
  return std::abs(int(a) - int(b)) <= 1;
}

static void
expectPixel(const char *backend, const char *what, const uint8_t *image, uint32_t width, uint32_t x, uint32_t y,
            const uint8_t (&expected)[4]) {
  const uint8_t *p = image + (y * width + x) * 4;
  bool pixel_matches =
      near(p[0], expected[0]) && near(p[1], expected[1]) && near(p[2], expected[2]) && near(p[3], expected[3]);
  CHECK(pixel_matches, "%s %s (%u,%u): %u %u %u %u, expected %u %u %u %u", backend, what, x, y, p[0], p[1], p[2], p[3],
        expected[0], expected[1], expected[2], expected[3]);
}

static uint8_t
unorm(float v) {
  return uint8_t(std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f));
}

// what the CPU says the frame drew, at sample points; each readback's checks under its KnownScope
static void
expectFrame(const char *backend, const Resources &r, bool presented) {
  auto *scene = static_cast<const uint8_t *>(r.rb_scene.contents);
  {
    KnownScope scope(backend, "scene");
    // A: the texture's quadrants over the left half (4 px per texel across, 8 down)
    expectPixel(backend, "scene A", scene, kSize, 8, 16, kQuadrant[0]);
    expectPixel(backend, "scene A", scene, kSize, 24, 16, kQuadrant[1]);
    expectPixel(backend, "scene A", scene, kSize, 8, 48, kQuadrant[2]);
    expectPixel(backend, "scene A", scene, kSize, 24, 48, kQuadrant[3]);
    // B: red * 0.6 over the top-right quadrant
    const uint8_t b[4] = {unorm(0.6f), 0, 0, 255};
    expectPixel(backend, "scene B", scene, kSize, 48, 16, b);
    // D: depth-rejected except in the bottom-right quadrant: (0, 0.4, 0)
    const uint8_t d[4] = {0, unorm(0.4f), 0, 255};
    expectPixel(backend, "scene D", scene, kSize, 40, 60, d);
    // C: (0.25, 0, 1, 1) blended 50/50 with D, instance 1 at (48,48), instance 2 at (56,48)
    const uint8_t cc[4] = {unorm(0.125f), unorm(0.2f), unorm(0.5f), 255};
    expectPixel(backend, "scene C", scene, kSize, 48, 48, cc);
    expectPixel(backend, "scene C", scene, kSize, 56, 48, cc);
  }
  {
    KnownScope scope(backend, "rows");
    // row i's own 4 KB constants
    auto *rows = static_cast<const uint8_t *>(r.rb_rows.contents);
    for (uint32_t i = 0; i < kSize; i++) {
      const uint8_t e[4] = {unorm(float(i) / 63.0f), unorm(1.0f - float(i) / 63.0f), uint8_t(i % 2 ? 255 : 0),
                            uint8_t(i)};
      expectPixel(backend, "rows", rows, kSize, (i * 7) % kSize, i, e);
    }
  }
  {
    KnownScope scope(backend, "mip 2");
    // each quadrant's colour (a box filter of uniform 8x8 blocks)
    auto *mip = static_cast<const uint8_t *>(r.rb_mip.contents);
    expectPixel(backend, "mip 2", mip, kMip / 4, 0, 0, kQuadrant[0]);
    expectPixel(backend, "mip 2", mip, kMip / 4, 3, 0, kQuadrant[1]);
    expectPixel(backend, "mip 2", mip, kMip / 4, 0, 3, kQuadrant[2]);
    expectPixel(backend, "mip 2", mip, kMip / 4, 3, 3, kQuadrant[3]);
  }
  {
    KnownScope scope(backend, "compute");
    // (packed scene pixel ^ xor) + add, every word; the tail is FillBuffer's
    auto *out = static_cast<const uint32_t *>(r.out.contents);
    uint32_t bad = 0, first = 0;
    for (uint32_t i = 0; i < kOutWords; i++) {
      uint32_t packed;
      std::memcpy(&packed, scene + i * 4, 4);
      if (out[i] != (packed ^ kXor) + kAdd && !bad++)
        first = i;
    }
    CHECK(!bad, "%s compute: %u words wrong, first %u: %08x, expected %08x", backend, bad, first, out[first],
          [&] {
            uint32_t packed;
            std::memcpy(&packed, scene + first * 4, 4);
            return (packed ^ kXor) + kAdd;
          }());
    auto *tail = reinterpret_cast<const uint8_t *>(out + kOutWords);
    bool filled = true;
    for (uint32_t i = 0; i < kOutTail; i++)
      filled = filled && tail[i] == 0xab;
    CHECK(filled, "%s compute: FillBuffer's tail isn't 0xab", backend);
  }
  if (presented) {
    KnownScope scope(backend, "drawable");
    // BGRA: the back buffer (the scene) times the gain
    auto *present = static_cast<const uint8_t *>(r.rb_present.contents);
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < kSize * kSize; i++) {
      const uint8_t *s = scene + i * 4, *p = present + i * 4;
      uint8_t e[4] = {s[2], s[1], unorm(s[0] / 255.0f * kGain[0]), s[3]};
      if (!(near(p[0], e[0]) && near(p[1], e[1]) && near(p[2], e[2]) && near(p[3], e[3])))
        wrong++;
    }
    CHECK(!wrong, "%s drawable: %u of %u pixels aren't the scene times the gain", backend, wrong, kSize * kSize);
  }
}


// Metal 3 against Metal 4, byte for byte
static void
compare(const char *what, id<MTLBuffer> m3, id<MTLBuffer> m4, uint64_t length, uint32_t width) {
  auto *a = static_cast<const uint8_t *>(m3.contents), *b = static_cast<const uint8_t *>(m4.contents);
  uint64_t differ = 0, first = 0;
  for (uint64_t i = 0; i < length; i++)
    if (a[i] != b[i] && !differ++)
      first = i;
  const char *issue = known(what);
  if (issue) {
    if (differ) {
      xfailed++;
      std::printf("XFAIL %s: Metal 3 and Metal 4 differ in %llu bytes (%s)\n", what, (unsigned long long)differ,
                  issue);
    } else {
      std::printf("XPASS %s: matches now; remove its kKnown entry (%s)\n", what, issue);
      failures++;
    }
    return;
  }
  if (differ) {
    uint64_t px = first / 4; // a pixel, or a word of a buffer (width 0)
    std::printf("FAIL %s: Metal 3 and Metal 4 differ in %llu of %llu bytes; first at byte %llu (%s %llu,%llu): "
                "Metal 3 %u %u %u %u, Metal 4 %u %u %u %u\n",
                what, (unsigned long long)differ, (unsigned long long)length, (unsigned long long)first,
                width ? "pixel" : "word", (unsigned long long)(width ? px % width : px),
                (unsigned long long)(width ? px / width : 0), a[px * 4], a[px * 4 + 1], a[px * 4 + 2], a[px * 4 + 3], b[px * 4], b[px * 4 + 1], b[px * 4 + 2],
                b[px * 4 + 3]);
    failures++;
  } else {
    std::printf("ok %s: identical (%llu bytes)\n", what, (unsigned long long)length);
  }
}

// every command buffer completed without an error
static void
waitMetal3(std::vector<WMT::CommandBuffer> &committed, size_t expected, const char *what) {
  CHECK(committed.size() == expected, "%s: %zu command buffers, expected %zu", what, committed.size(), expected);
  for (auto &cmdbuf : committed) {
    cmdbuf.waitUntilCompleted();
    auto status = cmdbuf.status();
    WMT::Error error = cmdbuf.error();
    CHECK(status == WMTCommandBufferStatusCompleted && !error, "%s: status %llu, error %s", what,
          (unsigned long long)status, error ? error.description().getUTF8String().c_str() : "none");
  }
}

// the chunk's commits completed without an error; then it retires (its slot's residency set emptied)
static void
waitMetal4(Mtl4Queue &queue, uint64_t seq, size_t expected, const char *what) {
  const auto &commits = queue.wait(seq);
  CHECK(commits.size() == expected, "%s: %zu commits, expected %zu", what, commits.size(), expected);
  for (auto *commit : commits) {
    CHECK(commit->done(), "%s: a commit isn't done after wait()", what);
    WMT::Error error = commit->error();
    CHECK(!error, "%s: commit error %s", what, error ? error.description().getUTF8String().c_str() : "");
  }
  queue.retire(seq);
}

static CAMetalLayer *
newLayer(id<MTLDevice> device) {
  CAMetalLayer *layer = [CAMetalLayer layer];
  layer.device = device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = NO; // the test reads the drawable back
  layer.drawableSize = CGSizeMake(kSize, kSize);
  return layer;
}

int
main() {
  bool presented = false; // both backends presented and their drawables were compared
  @autoreleasepool {
    Fixture fx;
    fx.device = MTLCreateSystemDefaultDevice();
    if (!fx.device) {
      std::printf("skip: no Metal device\n");
      return 77; // X1: skipped, not passed
    }
    WMT::Device device{H(fx.device)};
    auto support = MTLDevice_metal4Support(device.handle);
    if (support != WMTMetal4Supported) {
      std::printf("skip: no Metal 4 (%u)\n", unsigned(support));
      return 77;
    }
    std::printf("device: %s (%s)\n", fx.device.name.UTF8String,
#if defined(__x86_64__)
                "x86_64, Rosetta"
#else
                "arm64"
#endif
    );
    if (!makeFixture(fx))
      return 1;

    // --- Metal 3
    Resources r3 = makeResources(fx.device);
    Stream s3;
    buildFrame(s3, fx, r3);
    RunResult run3;
    {
      WMT::CommandQueue queue{MTLDevice_newCommandQueue(device.handle, 16)};
      CHECK(queue, "Metal 3 queue");
      CAMetalLayer *layer = newLayer(fx.device);
      {
        Metal3Target load(queue);
        encodeLoad(load, s3);
        waitMetal3(load.committed, 1, "Metal 3 load");
      }
      Metal3Target target(queue);
      run3 = encodeFrame(target, s3, fx, r3, WMT::MetalLayer{H(layer)});
      // gw2 (#107): two either way: split() committed the frame's, end() commits the present's (empty
      // without a drawable)
      waitMetal3(target.committed, 2, "Metal 3 frame");
      queue.release();
    }

    // --- Metal 4
    Resources r4 = makeResources(fx.device);
    Stream s4;
    buildFrame(s4, fx, r4);
    RunResult run4;
    {
      Mtl4Queue queue(device, 2);
      CHECK(queue.valid(), "Metal 4 queue");
      if (!queue.valid())
        return 1;
      CAMetalLayer *layer = newLayer(fx.device);
      WMT::MetalLayer wmt_layer{H(layer)};
      // the layer's own set keeps the drawables resident (one layer, so on the queue here; the
      // presenter names it on the present's command buffer, #184)
      if (auto set = wmt_layer.residencySet())
        queue.queue().addResidencySet(WMT::ResidencySet{set.handle});
      std::vector<id<MTLBuffer>> arenas; // the bytes arenas, alive until the chunk retired
      Metal4Target *current = nullptr;
      uint32_t arena_calls = 0;
      auto bytes = [&](uint64_t size) {
        id<MTLBuffer> b = [fx.device newBufferWithLength:size
                                                 options:MTLResourceStorageModeShared |
                                                         MTLResourceHazardTrackingModeUntracked];
        arenas.push_back(b);
        arena_calls++;
        if (current && current->in_rows)
          current->rows_refills++;
        return Mtl4Encoding::Bytes{WMT::Buffer{H(b)}, 0, false};
      };
      // the load chunk (ring slot 0): its residency set is emptied when it retires
      {
        Mtl4Encoding encoding(queue, 0, bytes);
        Metal4Target target{encoding};
        encodeLoad(target, s4);
      }
      waitMetal4(queue, 0, 1, "Metal 4 load");
      // the frame (ring slot 1)
      constexpr uint64_t seq = 1;
      WMTMtl4BindingState state{};
      {
        Mtl4Encoding encoding(queue, seq, bytes);
        Metal4Target target{encoding};
        current = &target;
        arena_calls = 0;
        run4 = encodeFrame(target, s4, fx, r4, wmt_layer);
        state = encoding.state();
        std::printf("Metal 4: bytes arenas %u (%u refills inside the rows pass), residency adds %u, blit barriers %u\n",
                    arena_calls, target.rows_refills, state.residency_adds, state.blit_barriers);
        CHECK(target.rows_refills >= 1, "Metal 4: the rows pass's 256 KB of bytes never refilled the arena");
        current = nullptr;
      }
      CHECK(state.unsupported == 0, "Metal 4: %u unsupported packets", state.unsupported);
      CHECK(state.use_resource_dropped == 0, "Metal 4: %u UseResource dropped", state.use_resource_dropped);
      CHECK(state.bytes_overflow == 0, "Metal 4: %u Set*Bytes overflowed", state.bytes_overflow);
      CHECK(state.residency_adds > 0, "Metal 4: nothing went into the chunk's residency set");
      waitMetal4(queue, seq, run4.presented ? 2 : 1, "Metal 4 frame");
      queue.waitIdle();
    }

    // --- the pairing
    CHECK(run3.types == run4.types, "the two backends were given different packet streams");
    CHECK(run3.presented == run4.presented, "a drawable for one backend only");
    presented = run3.presented && run4.presented;
    size_t packets = 0;
    for (auto &t : run3.types)
      packets += t.size();
    std::printf("stream: %zu packets in %zu encoders\n", packets, run3.types.size());

    expectFrame("Metal 3", r3, run3.presented);
    expectFrame("Metal 4", r4, run4.presented);
    compare("scene", r3.rb_scene, r4.rb_scene, kSize * kSize * 4, kSize);
    compare("rows", r3.rb_rows, r4.rb_rows, kSize * kSize * 4, kSize);
    compare("mip 2", r3.rb_mip, r4.rb_mip, (kMip / 4) * (kMip / 4) * 4, kMip / 4);
    compare("compute", r3.out, r4.out, kOutWords * 4 + kOutTail, 0);
    if (run3.presented && run4.presented)
      compare("drawable", r3.rb_present, r4.rb_present, kSize * kSize * 4, kSize);
  }
  std::printf("%s (%d failures, %d xfail)\n", failures ? "FAILED" : "passed", failures, xfailed);
  if (failures)
    return 1;
  // gw2 (#107): everything else compared, but the present pass wasn't: not a pass
  if (!presented) {
    std::printf("skip: the layer gave no drawable without a window; the present pass wasn't compared\n");
    return 77;
  }
  return 0;
}
