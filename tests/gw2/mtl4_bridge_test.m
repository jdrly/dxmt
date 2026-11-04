// gw2 (#32): native test and probe of the Metal 4 bridge (src/winemetal/unix/mtl4.c, slots 155-178).
// It calls the bridge's unix entry points directly, as winemetal.so does for the PE side, on a real
// Metal 4 queue: no Wine, no game, no window. Not part of the build; run it on the Mac:
//
//   T=$(mktemp -d)
//   for arch in x86_64 arm64; do
//     clang -x objective-c -std=gnu17 -fblocks -fno-objc-arc -O1 -arch $arch -mmacosx-version-min=26.0 \
//       -Isrc/winemetal -Isrc/winemetal/unix -Isrc/airconv \
//       src/winemetal/unix/mtl4.c tests/gw2/mtl4_bridge_test.m \
//       -framework Metal -framework QuartzCore -framework Foundation -lsqlite3 -o $T/mtl4_test_$arch
//   done
//   $T/mtl4_test_x86_64                    # under Rosetta, as winemetal.so runs in the game
//   MTL_DEBUG_LAYER=1 $T/mtl4_test_arm64   # with Metal's validation layer
//
// 1. Replay: a device-built render pipeline (newRenderPipelineStateWithDescriptor, as DXMT builds
//    them) draws through today's packets on an MTL4 render encoder: buffers bound by Set*Buffer and
//    moved by Set*BufferOffset, a texture reached only through an argument buffer (gpuResourceID, as
//    DXMT's), Set*Bytes, an indexed draw at an index buffer offset, a dropped UseResource. A fence
//    orders it before a blit replay (texture readback, then a fill and a dependent copy) on an MTL4
//    compute encoder; a compute replay runs two dispatches with a barrier between them. Everything
//    is resident through one residency set on the queue, the pipelines included (on Metal 4 they
//    are allocations that need residency too). One commit with feedback and a shared
//    event; the pixels and buffers are checked.
//    Then the same again with nothing made resident by hand: the replays put every allocation the
//    packets name (pipelines, buffers, textures, attachments, UseResource's) into a chunk set (part 3).
// 2. Part 4: a fence orders two separately committed command buffers (A fills 64 MB, B copies its
//    end), as the present split and queue-level events rely on; and the present path on a layer
//    without a window (waitForDrawable, a pass, signalDrawable, present at a deadline), skipped if
//    the layer gives no drawable.
// 3. Probe (the issue's question, with --metallibs <copy of a DXMT shaders_*.db> [--limit n]):
//    airconv's metallibs (GW2's own shaders, from DXMT's shader cache) load; their vertex functions
//    (with a constant fragment function of the test's) and kernel functions build pipelines both
//    through the device (today's path) and through MTL4Compiler; the
//    device-built ones bind on MTL4 encoders (SetPSO through the replay) and the commit reports no
//    error. Fragment functions are counted (they need their vertex shader to link). Copy the
//    database first; never open the live one while GW2 runs.

#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "mtl4.h"

static int failures;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                       \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

#define H(x) ((obj_handle_t)(x))

static const char *kSource =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VArgs { device const float2 *pos; };\n"
    "struct FArgs { texture2d<float> tex; };\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut vs(uint vid [[vertex_id]], constant VArgs &args [[buffer(1)]]) {\n"
    "  VOut o; o.pos = float4(args.pos[vid], 0, 1); return o;\n"
    "}\n"
    "vertex VOut vs_position_only(uint vid [[vertex_id]]) { VOut o; o.pos = float4(0, 0, 0, 1); return o; }\n"
    "fragment float4 fs_constant() { return float4(0, 0, 0, 1); }\n"
    "fragment float4 fs(constant float4 &color [[buffer(0)]], constant FArgs &args [[buffer(1)]],\n"
    "                   constant float4 &add [[buffer(2)]]) {\n"
    "  return color * args.tex.read(uint2(0, 0)) + add;\n"
    "}\n"
    "kernel void cs_fill(device uint *out [[buffer(0)]], constant uint &base [[buffer(1)]],\n"
    "                    uint i [[thread_position_in_grid]]) { out[i] = base + i; }\n"
    "kernel void cs_double(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i] *= 2; }\n";

/* a chain of packets in one block of memory, as DXMT's encode thread builds them */
struct chain {
  char data[4096];
  size_t used;
  struct wmtcmd_base *last;
  struct wmtcmd_base *head;
};

static void *
chain_add(struct chain *c, size_t size, uint16_t type) {
  size = (size + 15) & ~(size_t)15;
  if (c->used + size > sizeof(c->data))
    abort();
  struct wmtcmd_base *cmd = (struct wmtcmd_base *)(c->data + c->used);
  memset(cmd, 0, size);
  c->used += size;
  cmd->type = type;
  if (c->last)
    c->last->next.ptr = cmd;
  else
    c->head = cmd;
  c->last = cmd;
  return cmd;
}

struct mtl4 {
  id<MTLDevice> device;
  obj_handle_t queue, allocator, cmdbuf;
  id<MTLResidencySet> residency;
  /* part 3: residency only from the replays, into this set (the chunk's), nothing added by hand */
  id<MTLResidencySet> chunk;
};

static obj_handle_t
new_table(id<MTLDevice> device, uint32_t buffers, uint32_t textures) {
  struct WMTArgTableInfo info = {.max_buffers = buffers, .max_textures = textures, .max_samplers = 0};
  struct unixcall_mtl4_new_argtable p = {.device = H(device)};
  p.info.ptr = &info;
  _MTLDevice_newArgumentTable(&p);
  if (p.ret_error)
    printf("newArgumentTable: %s\n", [((NSError *)p.ret_error).description UTF8String]);
  return p.ret;
}

static obj_handle_t
render_encoder(struct mtl4 *m, struct WMTRenderPassInfo *info, struct WMTMtl4BindingState *state) {
  struct unixcall_mtl4_new_encoder p = {.cmdbuf = m->cmdbuf};
  p.info.ptr = info;
  p.state.ptr = state;
  _MTL4CommandBuffer_renderCommandEncoder(&p);
  return p.ret;
}

static obj_handle_t
compute_encoder(struct mtl4 *m, struct WMTMtl4BindingState *state) {
  struct unixcall_mtl4_new_encoder p = {.cmdbuf = m->cmdbuf};
  p.state.ptr = state;
  _MTL4CommandBuffer_computeCommandEncoder(&p);
  return p.ret;
}

static void
replay(NTSTATUS (*fn)(void *), obj_handle_t encoder, struct WMTMtl4BindingState *state, struct chain *c) {
  struct unixcall_mtl4_encode p = {.encoder = encoder};
  p.state.ptr = state;
  p.cmd_head.ptr = c->head;
  fn(&p);
}

static void
begin(struct mtl4 *m) {
  struct unixcall_generic_obj_obj_noret p = {.handle = m->cmdbuf, .arg = m->allocator};
  _MTL4CommandBuffer_beginCommandBuffer(&p);
}

/* ends the command buffer, commits it with feedback and an event, waits; true if it completed */
static bool
commit_and_wait(struct mtl4 *m, struct WMTMtl4CommitFeedback *fb, const char *what) {
  if (m->chunk) {
    [m->chunk commit];
    struct unixcall_generic_obj_obj_noret use = {.handle = m->cmdbuf, .arg = H(m->chunk)};
    _MTL4CommandBuffer_useResidencySet(&use);
  }
  struct unixcall_generic_obj_noret end = {.handle = m->cmdbuf};
  _MTL4CommandBuffer_endCommandBuffer(&end);
  [m->residency commit];
  id<MTLSharedEvent> event = [m->device newSharedEvent];
  memset(fb, 0, sizeof(*fb));
  struct unixcall_mtl4_queue_commit p = {.queue = m->queue, .count = 1, .event = H(event), .event_value = 1};
  p.cmdbufs.ptr = &m->cmdbuf;
  p.feedback.ptr = fb;
  _MTL4CommandQueue_commit(&p);
  bool done = [event waitUntilSignaledValue:1 timeoutMS:10000];
  if (!done) {
    /* the GPU work and the feedback handler may still run: reading the feedback, resetting the
       allocator or releasing anything they use would race them, so stop here without cleanup */
    printf("FAIL %s: the commit's event wasn't signalled in 10 s; exiting without cleanup\n", what);
    fflush(stdout);
    _Exit(EXIT_FAILURE);
  }
  CHECK(atomic_load_explicit((_Atomic uint64_t *)&fb->done, memory_order_acquire) == 1,
        "%s: feedback not done when the event was", what);
  CHECK(!fb->error, "%s: GPU error %s", what, fb->error ? [((NSError *)fb->error).description UTF8String] : "");
  CHECK(fb->gpu_start > 0 && fb->gpu_end >= fb->gpu_start, "%s: GPU times %f-%f", what, fb->gpu_start, fb->gpu_end);
  struct unixcall_generic_obj_uint64_ret reset = {.handle = m->allocator};
  _MTL4CommandAllocator_reset(&reset);
  [event release];
  return done;
}

static id<MTLBuffer>
resident_buffer(struct mtl4 *m, NSUInteger length) {
  id<MTLBuffer> b = [m->device newBufferWithLength:length
                                           options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
  memset(b.contents, 0, length);
  if (!m->chunk)
    [m->residency addAllocation:b];
  return b;
}

static id<MTLTexture>
resident_texture(struct mtl4 *m, MTLTextureUsage usage) {
  MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                               width:4
                                                                              height:4
                                                                           mipmapped:NO];
  d.usage = usage;
  d.storageMode = MTLStorageModePrivate;
  d.hazardTrackingMode = MTLHazardTrackingModeUntracked;
  id<MTLTexture> t = [m->device newTextureWithDescriptor:d];
  if (!m->chunk)
    [m->residency addAllocation:t];
  return t;
}

static void
test_replay(struct mtl4 *m) {
  NSError *err = nil;
  id<MTLLibrary> lib = [m->device newLibraryWithSource:@(kSource) options:nil error:&err];
  CHECK(lib, "shader source: %s", err.description.UTF8String);
  if (!lib)
    return;
  MTLRenderPipelineDescriptor *rd = [[MTLRenderPipelineDescriptor alloc] init];
  rd.vertexFunction = [[lib newFunctionWithName:@"vs"] autorelease];
  rd.fragmentFunction = [[lib newFunctionWithName:@"fs"] autorelease];
  rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  id<MTLRenderPipelineState> pso = [m->device newRenderPipelineStateWithDescriptor:rd error:&err];
  [rd release];
  CHECK(pso, "render pipeline: %s", err.description.UTF8String);
  id<MTLComputePipelineState> cs_fill =
      [m->device newComputePipelineStateWithFunction:[[lib newFunctionWithName:@"cs_fill"] autorelease] error:&err];
  id<MTLComputePipelineState> cs_double =
      [m->device newComputePipelineStateWithFunction:[[lib newFunctionWithName:@"cs_double"] autorelease] error:&err];
  CHECK(cs_fill && cs_double, "compute pipelines: %s", err.description.UTF8String);
  if (!pso || !cs_fill || !cs_double)
    return;
  /* Metal 4: pipelines are allocations too and must be resident (Apple, WWDC25 254; d9mt #63); with
     m->chunk the replays add them (SetPSO) */
  if (!m->chunk) {
    [m->residency addAllocation:pso];
    [m->residency addAllocation:cs_fill];
    [m->residency addAllocation:cs_double];
  }

  /* resources: everything the GPU touches is in the residency set (Metal 4 has no useResource) */
  id<MTLBuffer> argbuf = resident_buffer(m, 4096);
  id<MTLBuffer> positions = resident_buffer(m, 256);
  id<MTLBuffer> indices = resident_buffer(m, 64);
  id<MTLBuffer> bytes = resident_buffer(m, 4096);
  id<MTLBuffer> readback = resident_buffer(m, 4 * 4 * 4);
  id<MTLBuffer> fill_a = resident_buffer(m, 64), fill_b = resident_buffer(m, 64);
  id<MTLBuffer> out = resident_buffer(m, 64 * 4);
  id<MTLBuffer> staging = resident_buffer(m, 4 * 4 * 4);
  id<MTLTexture> source = resident_texture(m, MTLTextureUsageShaderRead);
  id<MTLTexture> target = resident_texture(m, MTLTextureUsageRenderTarget);
  id<MTLFence> fence = [m->device newFence];

  float pos[3][2] = {{-1, -1}, {3, -1}, {-1, 3}}; /* one triangle over the whole target */
  memcpy(positions.contents, pos, sizeof(pos));
  uint32_t idx[4] = {7, 0, 1, 2}; /* drawn from offset 4: the first is junk */
  memcpy(indices.contents, idx, sizeof(idx));
  uint8_t texel[4 * 4 * 4];
  for (int i = 0; i < 16; i++) {
    texel[4 * i + 0] = 128, texel[4 * i + 1] = 128, texel[4 * i + 2] = 255, texel[4 * i + 3] = 255;
  }
  memcpy(staging.contents, texel, sizeof(texel));
  /* the argument buffer, as DXMT writes it: a pointer at 0 (vertex), a constant buffer at 256, a
     texture's resource ID at 512 (fragment) */
  uint64_t pos_address = positions.gpuAddress;
  memcpy((char *)argbuf.contents, &pos_address, 8);
  float color[4] = {1.0f, 1.0f, 0.25f, 1.0f};
  memcpy((char *)argbuf.contents + 256, color, sizeof(color));
  MTLResourceID tex_id = source.gpuResourceID;
  memcpy((char *)argbuf.contents + 512, &tex_id, 8);
  float add[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  add[0] = 0.25f;

  struct WMTMtl4BindingState render_state, blit_state, compute_state;
  memset(&render_state, 0, sizeof(render_state));
  render_state.tables[WMTMtl4TableVertex] = new_table(m->device, 8, 0);
  render_state.tables[WMTMtl4TableFragment] = new_table(m->device, 8, 0);
  render_state.bytes_buffer = H(bytes);
  render_state.bytes_offset = 8; /* the replay aligns it up to 256 */
  render_state.bytes_end = 4096;
  memset(&blit_state, 0, sizeof(blit_state));
  memset(&compute_state, 0, sizeof(compute_state));
  compute_state.tables[WMTMtl4TableCompute] = new_table(m->device, 8, 0);
  render_state.residency_set = blit_state.residency_set = compute_state.residency_set = H(m->chunk);
  compute_state.bytes_buffer = H(bytes);
  compute_state.bytes_offset = 1024;
  compute_state.bytes_end = 4096;
  CHECK(render_state.tables[0] && render_state.tables[1] && compute_state.tables[0], "argument tables");

  begin(m);
  /* upload the source texture (blit replay; the fence orders it before the render pass) */
  {
    obj_handle_t enc = compute_encoder(m, &blit_state);
    struct chain c = {0};
    struct wmtcmd_blit_copy_from_buffer_to_texture *copy =
        chain_add(&c, sizeof(*copy), WMTBlitCommandCopyFromBufferToTexture);
    copy->src = H(staging);
    copy->bytes_per_row = 16;
    copy->bytes_per_image = 64;
    copy->size = (struct WMTSize){4, 4, 1};
    copy->dst = H(source);
    struct wmtcmd_blit_fence_op *update = chain_add(&c, sizeof(*update), WMTBlitCommandUpdateFence);
    update->fence = H(fence);
    replay(_MTL4ComputeCommandEncoder_encodeBlitCommands, enc, &blit_state, &c);
    [(id<MTL4ComputeCommandEncoder>)enc endEncoding];
  }
  /* the render pass */
  {
    struct WMTRenderPassInfo info;
    memset(&info, 0, sizeof(info));
    info.colors[0].texture = H(target);
    info.colors[0].load_action = WMTLoadActionClear;
    info.colors[0].store_action = WMTStoreActionStore;
    info.default_raster_sample_count = 1;
    info.render_target_width = 4;
    info.render_target_height = 4;
    obj_handle_t enc = render_encoder(m, &info, &render_state);
    CHECK(enc, "render encoder");
    struct chain c = {0};
    struct wmtcmd_render_fence_op *wait = chain_add(&c, sizeof(*wait), WMTRenderCommandWaitForFence);
    wait->fence = H(fence);
    wait->stages = WMTRenderStageFragment;
    struct wmtcmd_render_setbuffer *vb = chain_add(&c, sizeof(*vb), WMTRenderCommandSetVertexBuffer);
    vb->buffer = H(argbuf), vb->offset = 128, vb->index = 1; /* moved to 0 below */
    struct wmtcmd_render_setbufferoffset *vbo = chain_add(&c, sizeof(*vbo), WMTRenderCommandSetVertexBufferOffset);
    vbo->offset = 0, vbo->index = 1;
    struct wmtcmd_render_setbuffer *fb0 = chain_add(&c, sizeof(*fb0), WMTRenderCommandSetFragmentBuffer);
    fb0->buffer = H(argbuf), fb0->offset = 0, fb0->index = 0;
    struct wmtcmd_render_setbufferoffset *fbo = chain_add(&c, sizeof(*fbo), WMTRenderCommandSetFragmentBufferOffset);
    fbo->offset = 256, fbo->index = 0;
    struct wmtcmd_render_setbuffer *fb1 = chain_add(&c, sizeof(*fb1), WMTRenderCommandSetFragmentBuffer);
    fb1->buffer = H(argbuf), fb1->offset = 512, fb1->index = 1;
    struct wmtcmd_render_setbytes *fbytes = chain_add(&c, sizeof(*fbytes), WMTRenderCommandSetFragmentBytes);
    fbytes->bytes.ptr = add, fbytes->length = sizeof(add), fbytes->index = 2;
    struct wmtcmd_render_useresource *use = chain_add(&c, sizeof(*use), WMTRenderCommandUseResource);
    use->resource = H(source), use->usage = WMTResourceUsageRead, use->stages = WMTRenderStageFragment;
    struct wmtcmd_render_useresource *use_pos = chain_add(&c, sizeof(*use_pos), WMTRenderCommandUseResource);
    use_pos->resource = H(positions), use_pos->usage = WMTResourceUsageRead, use_pos->stages = WMTRenderStageVertex;
    struct wmtcmd_render_setpso *setpso = chain_add(&c, sizeof(*setpso), WMTRenderCommandSetPSO);
    setpso->pso = H(pso);
    struct wmtcmd_render_setviewport *vp = chain_add(&c, sizeof(*vp), WMTRenderCommandSetViewport);
    vp->viewport = (struct WMTViewport){0, 0, 4, 4, 0, 1};
    struct wmtcmd_render_draw_indexed *draw = chain_add(&c, sizeof(*draw), WMTRenderCommandDrawIndexed);
    draw->primitive_type = WMTPrimitiveTypeTriangle;
    draw->index_type = WMTIndexTypeUInt32;
    draw->index_count = 3;
    draw->index_buffer = H(indices);
    draw->index_buffer_offset = 4;
    draw->instance_count = 1;
    struct wmtcmd_render_fence_op *update = chain_add(&c, sizeof(*update), WMTRenderCommandUpdateFence);
    update->fence = H(fence);
    update->stages = WMTRenderStageFragment;
    replay(_MTL4RenderCommandEncoder_encodeCommands, enc, &render_state, &c);
    [(id<MTL4RenderCommandEncoder>)enc endEncoding];
  }
  /* blit replay: read the target back; fill, then a copy that depends on the fill */
  {
    obj_handle_t enc = compute_encoder(m, &blit_state);
    struct chain c = {0};
    struct wmtcmd_blit_fence_op *wait = chain_add(&c, sizeof(*wait), WMTBlitCommandWaitForFence);
    wait->fence = H(fence);
    struct wmtcmd_blit_copy_from_texture_to_buffer *rb =
        chain_add(&c, sizeof(*rb), WMTBlitCommandCopyFromTextureToBuffer);
    rb->src = H(target);
    rb->size = (struct WMTSize){4, 4, 1};
    rb->dst = H(readback);
    rb->bytes_per_row = 16;
    rb->bytes_per_image = 64;
    struct wmtcmd_blit_fillbuffer *fill = chain_add(&c, sizeof(*fill), WMTBlitCommandFillBuffer);
    fill->buffer = H(fill_a), fill->offset = 0, fill->length = 64, fill->value = 0x5a;
    struct wmtcmd_blit_copy_from_buffer_to_buffer *copy =
        chain_add(&c, sizeof(*copy), WMTBlitCommandCopyFromBufferToBuffer);
    copy->src = H(fill_a), copy->dst = H(fill_b), copy->copy_length = 64;
    replay(_MTL4ComputeCommandEncoder_encodeBlitCommands, enc, &blit_state, &c);
    [(id<MTL4ComputeCommandEncoder>)enc endEncoding];
  }
  /* compute replay: out[i] = base + i, a barrier, then out[i] *= 2 */
  {
    obj_handle_t enc = compute_encoder(m, &compute_state);
    struct chain c = {0};
    uint32_t base = 1000;
    struct wmtcmd_compute_setpso *p1 = chain_add(&c, sizeof(*p1), WMTComputeCommandSetPSO);
    p1->pso = H(cs_fill), p1->threadgroup_size = (struct WMTSize){64, 1, 1};
    struct wmtcmd_compute_setbuffer *b0 = chain_add(&c, sizeof(*b0), WMTComputeCommandSetBuffer);
    b0->buffer = H(out), b0->offset = 0, b0->index = 0;
    struct wmtcmd_compute_setbytes *sb = chain_add(&c, sizeof(*sb), WMTComputeCommandSetBytes);
    sb->bytes.ptr = &base, sb->length = 4, sb->index = 1;
    struct wmtcmd_compute_dispatch *d1 = chain_add(&c, sizeof(*d1), WMTComputeCommandDispatchThreads);
    d1->size = (struct WMTSize){64, 1, 1};
    chain_add(&c, sizeof(struct wmtcmd_compute_memory_barrier), WMTComputeCommandMemoryBarrier);
    struct wmtcmd_compute_setpso *p2 = chain_add(&c, sizeof(*p2), WMTComputeCommandSetPSO);
    p2->pso = H(cs_double), p2->threadgroup_size = (struct WMTSize){64, 1, 1};
    struct wmtcmd_compute_dispatch *d2 = chain_add(&c, sizeof(*d2), WMTComputeCommandDispatchThreads);
    d2->size = (struct WMTSize){64, 1, 1};
    replay(_MTL4ComputeCommandEncoder_encodeCommands, enc, &compute_state, &c);
    [(id<MTL4ComputeCommandEncoder>)enc endEncoding];
  }
  /* part 3: a bytes arena with room for two Set*Bytes and four in the stream: the replay stops at the
     third (resume), gets a new arena and goes on, as Metal.hpp's encoders do; out2[k] = 100 * k */
  id<MTLBuffer> out2 = resident_buffer(m, 64);
  int resumes = 0;
  {
    obj_handle_t enc = compute_encoder(m, &compute_state);
    id<MTLBuffer> small = resident_buffer(m, 512);
    compute_state.bytes_buffer = H(small), compute_state.bytes_offset = 0, compute_state.bytes_end = 512;
    static uint32_t bases[4] = {0, 100, 200, 300};
    struct chain c = {0};
    struct wmtcmd_compute_setpso *p = chain_add(&c, sizeof(*p), WMTComputeCommandSetPSO);
    p->pso = H(cs_fill), p->threadgroup_size = (struct WMTSize){1, 1, 1};
    for (int k = 0; k < 4; k++) {
      struct wmtcmd_compute_setbuffer *b = chain_add(&c, sizeof(*b), WMTComputeCommandSetBuffer);
      b->buffer = H(out2), b->offset = 4 * k, b->index = 0;
      struct wmtcmd_compute_setbytes *sb = chain_add(&c, sizeof(*sb), WMTComputeCommandSetBytes);
      sb->bytes.ptr = &bases[k], sb->length = 4, sb->index = 1;
      struct wmtcmd_compute_dispatch *d = chain_add(&c, sizeof(*d), WMTComputeCommandDispatchThreads);
      d->size = (struct WMTSize){1, 1, 1};
    }
    struct unixcall_mtl4_encode e = {.encoder = enc};
    e.state.ptr = &compute_state;
    e.cmd_head.ptr = c.head;
    _MTL4ComputeCommandEncoder_encodeCommands(&e);
    while (compute_state.resume.ptr && resumes < 8) {
      resumes++;
      CHECK(compute_state.bytes_needed >= 4, "bytes_needed=%llu", (unsigned long long)compute_state.bytes_needed);
      id<MTLBuffer> more = resident_buffer(m, 512);
      compute_state.bytes_buffer = H(more), compute_state.bytes_offset = 0, compute_state.bytes_end = 512;
      e.cmd_head.ptr = compute_state.resume.ptr;
      compute_state.resume.ptr = NULL;
      _MTL4ComputeCommandEncoder_encodeCommands(&e);
    }
    [(id<MTL4ComputeCommandEncoder>)enc endEncoding];
  }
  struct WMTMtl4CommitFeedback fb;
  if (commit_and_wait(m, &fb, "replay")) {
    const uint8_t *px = readback.contents;
    /* color * texel + add = (1*0.502+0.25, 1*0.502, 0.25*1, 1*1) */
    int want[4] = {192, 128, 64, 255};
    bool pixels_ok = true;
    for (int i = 0; i < 16 && pixels_ok; i++)
      for (int ch = 0; ch < 4; ch++)
        if (abs(px[4 * i + ch] - want[ch]) > 2)
          pixels_ok = false;
    CHECK(pixels_ok, "render replay drew (%u,%u,%u,%u), want about (192,128,64,255)", px[0], px[1], px[2], px[3]);
    const uint8_t *fbuf = fill_b.contents;
    bool copied = true;
    for (int i = 0; i < 64; i++)
      copied &= fbuf[i] == 0x5a;
    CHECK(copied, "blit replay: the copy after the fill read 0x%02x, want 0x5a", fbuf[0]);
    const uint32_t *o = out.contents;
    bool computed = true;
    for (uint32_t i = 0; i < 64; i++)
      computed &= o[i] == (1000 + i) * 2;
    CHECK(computed, "compute replay: out[1]=%u, want %u", o[1], (1000 + 1) * 2);
    const uint32_t *o2 = out2.contents;
    CHECK(resumes == 1 && o2[0] == 0 && o2[1] == 100 && o2[2] == 200 && o2[3] == 300,
          "bytes resume: %d resumes, out2 = %u %u %u %u (want 1 resume, 0 100 200 300)", resumes, o2[0], o2[1], o2[2],
          o2[3]);
  }
  if (m->chunk) {
    CHECK(render_state.use_resource_dropped == 0, "use_resource_dropped=%u, want 0 with a residency set",
          render_state.use_resource_dropped);
    /* render: argbuf, the bytes buffer, source, positions, target, the pipeline, indices; blit: staging,
       source, target, readback, fill_a, fill_b; compute: the pipelines, out, the bytes buffer */
    CHECK(render_state.residency_adds >= 7 && blit_state.residency_adds >= 6 && compute_state.residency_adds >= 4,
          "residency_adds render %u blit %u compute %u", render_state.residency_adds, blit_state.residency_adds,
          compute_state.residency_adds);
  } else {
    CHECK(render_state.use_resource_dropped == 2, "use_resource_dropped=%u, want 2", render_state.use_resource_dropped);
  }
  CHECK(!render_state.unsupported && !blit_state.unsupported && !compute_state.unsupported,
        "unsupported packets: render %u blit %u compute %u", render_state.unsupported, blit_state.unsupported,
        compute_state.unsupported);
  CHECK(!render_state.bytes_overflow && !compute_state.bytes_overflow, "bytes overflow");
  CHECK(render_state.bytes_offset == 256 + 16, "render bytes_offset=%llu, want 272",
        (unsigned long long)render_state.bytes_offset);
  CHECK(blit_state.blit_barriers == 2, "blit_barriers=%u, want 2 (readback, fill, copy)", blit_state.blit_barriers);
  printf("replay (%s): render, blit and compute packets on Metal 4 encoders: %s\n",
         m->chunk ? "residency from the replays only" : "everything resident by hand", failures ? "FAILED" : "ok");
  for (int i = 0; i < 4; i++) {
    if (render_state.tables[i])
      [(id)render_state.tables[i] release];
  }
  [(id)compute_state.tables[0] release];
  if (m->chunk) {
    [m->chunk removeAllAllocations];
    [m->chunk commit];
  } else {
    [m->residency removeAllocation:pso];
    [m->residency removeAllocation:cs_fill];
    [m->residency removeAllocation:cs_double];
    [m->residency commit];
  }
  [pso release];
  [cs_fill release];
  [cs_double release];
  [fence release];
  [lib release];
}

/* --- part 4: a fence across two commits, and a drawable */

static void
commit_one(struct mtl4 *m, obj_handle_t cmdbuf, struct WMTMtl4CommitFeedback *fb, id<MTLSharedEvent> event,
           uint64_t value) {
  struct unixcall_generic_obj_noret end = {.handle = cmdbuf};
  _MTL4CommandBuffer_endCommandBuffer(&end);
  memset(fb, 0, sizeof(*fb));
  struct unixcall_mtl4_queue_commit p = {.queue = m->queue, .count = 1, .event = H(event), .event_value = value};
  p.cmdbufs.ptr = &cmdbuf;
  p.feedback.ptr = fb;
  _MTL4CommandQueue_commit(&p);
}

static void
wait_value(id<MTLSharedEvent> event, uint64_t value, const char *what) {
  if (![event waitUntilSignaledValue:value timeoutMS:10000]) {
    printf("FAIL %s: not complete in 10 s; exiting without cleanup\n", what);
    fflush(stdout);
    _Exit(EXIT_FAILURE);
  }
}

/* the event is forward-only: a later commit's handler may raise it past an earlier record before
   that one's handler ran, so each record's own flag says it was written (acquire) */
static void
wait_done(struct WMTMtl4CommitFeedback *fb, const char *what) {
  for (int i = 0; i < 10000; i++) {
    if (atomic_load_explicit((_Atomic uint64_t *)&fb->done, memory_order_acquire))
      return;
    usleep(1000);
  }
  printf("FAIL %s: feedback not written in 10 s; exiting without cleanup\n", what);
  fflush(stdout);
  _Exit(EXIT_FAILURE);
}

/* DXMT orders the present split's two command buffers (and the buffers around queue-level events)
   with MTLFences only. Command buffer A fills 64 MB and updates a fence; B, committed separately,
   waits for it and copies the fill's last bytes: they must be the fill's. */
static void
test_cross_commit_fence(struct mtl4 *m) {
  const NSUInteger big_size = 64u << 20;
  id<MTLBuffer> big = [m->device newBufferWithLength:big_size
                                             options:MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked];
  id<MTLBuffer> small = resident_buffer(m, 16);
  [m->residency addAllocation:big];
  id<MTLFence> fence = [m->device newFence];
  id<MTLSharedEvent> event = [m->device newSharedEvent];
  struct unixcall_generic_obj_obj_ret nb = {.handle = H(m->device)};
  _MTLDevice_newMTL4CommandBuffer(&nb);
  obj_handle_t second = nb.ret;
  struct WMTMtl4BindingState state;
  memset(&state, 0, sizeof(state));
  struct WMTMtl4CommitFeedback fa, fbb;

  begin(m);
  {
    obj_handle_t enc = compute_encoder(m, &state);
    struct chain c = {0};
    struct wmtcmd_blit_fillbuffer *fill = chain_add(&c, sizeof(*fill), WMTBlitCommandFillBuffer);
    fill->buffer = H(big), fill->offset = 0, fill->length = big_size, fill->value = 0x7e;
    struct wmtcmd_blit_fence_op *update = chain_add(&c, sizeof(*update), WMTBlitCommandUpdateFence);
    update->fence = H(fence);
    replay(_MTL4ComputeCommandEncoder_encodeBlitCommands, enc, &state, &c);
    [(id<MTL4ComputeCommandEncoder>)enc endEncoding];
  }
  [m->residency commit];
  commit_one(m, m->cmdbuf, &fa, event, 1);

  {
    struct unixcall_generic_obj_obj_noret b = {.handle = second, .arg = m->allocator};
    _MTL4CommandBuffer_beginCommandBuffer(&b);
    struct unixcall_mtl4_new_encoder e = {.cmdbuf = second};
    e.state.ptr = &state;
    _MTL4CommandBuffer_computeCommandEncoder(&e);
    struct chain c = {0};
    struct wmtcmd_blit_fence_op *wait = chain_add(&c, sizeof(*wait), WMTBlitCommandWaitForFence);
    wait->fence = H(fence);
    struct wmtcmd_blit_copy_from_buffer_to_buffer *copy =
        chain_add(&c, sizeof(*copy), WMTBlitCommandCopyFromBufferToBuffer);
    copy->src = H(big), copy->src_offset = big_size - 16, copy->dst = H(small), copy->copy_length = 16;
    replay(_MTL4ComputeCommandEncoder_encodeBlitCommands, e.ret, &state, &c);
    [(id<MTL4ComputeCommandEncoder>)e.ret endEncoding];
  }
  commit_one(m, second, &fbb, event, 2);
  wait_value(event, 2, "cross-commit fence");
  wait_done(&fa, "cross-commit fence A");
  wait_done(&fbb, "cross-commit fence B");
  CHECK(!fa.error && !fbb.error, "cross-commit fence: GPU error");
  const uint8_t *bytes = small.contents;
  bool ok = true;
  for (int i = 0; i < 16; i++)
    ok &= bytes[i] == 0x7e;
  CHECK(ok, "cross-commit fence: the second command buffer read 0x%02x, want 0x7e (the fence didn't order it)",
        bytes[0]);
  printf("cross-commit fence: %s (A %.3f-%.3f ms, B %.3f-%.3f ms)\n", ok ? "ok" : "FAILED", fa.gpu_start * 1e3,
         fa.gpu_end * 1e3, fbb.gpu_start * 1e3, fbb.gpu_end * 1e3);
  struct unixcall_generic_obj_uint64_ret reset = {.handle = m->allocator};
  _MTL4CommandAllocator_reset(&reset);
  [(id)second release];
  [m->residency removeAllocation:big];
  [m->residency commit];
  [big release];
  [fence release];
  [event release];
}

/* the present path: waitForDrawable, a pass into the drawable (kept out of the chunk's set; the
   layer's set on the queue), commit, signalDrawable, present. Headless, a layer may give no
   drawable: then it's skipped. */
static void
test_drawable(struct mtl4 *m) {
  CAMetalLayer *layer = [CAMetalLayer layer];
  layer.device = m->device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.drawableSize = CGSizeMake(64, 64);
  id<CAMetalDrawable> drawable = [layer nextDrawable];
  if (!drawable) {
    printf("drawable: the layer gave none without a window: skipped\n");
    return;
  }
  struct unixcall_generic_obj_obj_ret ls = {.handle = H(layer)};
  _MetalLayer_residencySet(&ls);
  CHECK(ls.ret, "the layer has no residency set");
  struct unixcall_generic_obj_obj_noret add = {.handle = m->queue, .arg = ls.ret};
  if (ls.ret)
    _MTL4CommandQueue_addResidencySet(&add);
  struct unixcall_generic_obj_obj_noret wd = {.handle = m->queue, .arg = H(drawable)};
  _MTL4CommandQueue_waitForDrawable(&wd);
  id<MTLResidencySet> chunk = [m->device newResidencySetWithDescriptor:[[MTLResidencySetDescriptor new] autorelease]
                                                                 error:nil];
  struct WMTMtl4BindingState state;
  memset(&state, 0, sizeof(state));
  state.residency_set = H(chunk);
  state.flags = WMT_MTL4_NO_ATTACHMENT_RESIDENCY;
  begin(m);
  struct WMTRenderPassInfo info;
  memset(&info, 0, sizeof(info));
  info.colors[0].texture = H(drawable.texture);
  info.colors[0].load_action = WMTLoadActionClear;
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].clear_color = (struct WMTClearColor){1, 0, 0, 1};
  info.default_raster_sample_count = 1;
  info.render_target_width = 64;
  info.render_target_height = 64;
  obj_handle_t enc = render_encoder(m, &info, &state);
  [(id<MTL4RenderCommandEncoder>)enc endEncoding];
  CHECK(state.residency_adds == 0, "the drawable went into the chunk's set (%u adds)", state.residency_adds);
  [chunk commit];
  struct unixcall_generic_obj_obj_noret use = {.handle = m->cmdbuf, .arg = H(chunk)};
  _MTL4CommandBuffer_useResidencySet(&use);
  id<MTLSharedEvent> event = [m->device newSharedEvent];
  struct WMTMtl4CommitFeedback fb;
  commit_one(m, m->cmdbuf, &fb, event, 1);
  _MTL4CommandQueue_signalDrawable(&wd);
  struct unixcall_mtl4_drawable_present pr = {.drawable = H(drawable), .mode = WMTDrawablePresentWithDeadline,
                                              .value = 1.0 / 60};
  _MetalDrawable_present(&pr);
  wait_value(event, 1, "drawable");
  wait_done(&fb, "drawable");
  CHECK(!fb.error, "drawable: GPU error %s", fb.error ? [((NSError *)fb.error).description UTF8String] : "");
  printf("drawable: wait, pass, signal, present at a deadline: %s\n", fb.error ? "FAILED" : "ok");
  struct unixcall_generic_obj_uint64_ret reset = {.handle = m->allocator};
  _MTL4CommandAllocator_reset(&reset);
  if (ls.ret)
    _MTL4CommandQueue_removeResidencySet(&add);
  [chunk release];
  [event release];
}

/* --- the probe: airconv's metallibs from a copy of DXMT's shader cache */

static void
probe_metallibs(struct mtl4 *m, const char *db_path, int limit) {
  sqlite3 *db = NULL;
  char uri[4096];
  snprintf(uri, sizeof(uri), "file:%s?immutable=1", db_path);
  if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
    CHECK(0, "can't open %s", db_path);
    return;
  }
  char table[128] = "";
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'cache_%' "
                             "ORDER BY name DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW)
    snprintf(table, sizeof(table), "%s", sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  CHECK(table[0], "no cache_* table in %s", db_path);
  char sql[256];
  snprintf(sql, sizeof(sql), "SELECT value FROM %s LIMIT %d", table, limit);
  sqlite3_prepare_v2(db, sql, -1, &st, NULL);

  NSError *err = nil;
  id<MTLLibrary> own = [m->device newLibraryWithSource:@(kSource) options:nil error:&err];
  id<MTLFunction> position_only = [own newFunctionWithName:@"vs_position_only"];
  /* an airconv vertex function returns its outputs, so it needs rasterization and a fragment
     function: this one reads no varyings, so it links with any */
  id<MTLFunction> constant_fragment = [own newFunctionWithName:@"fs_constant"];
  MTL4LibraryFunctionDescriptor *constant_fd = [[MTL4LibraryFunctionDescriptor new] autorelease];
  constant_fd.name = @"fs_constant";
  constant_fd.library = own;
  id<MTL4Compiler> compiler = [m->device newCompilerWithDescriptor:[[MTL4CompilerDescriptor new] autorelease]
                                                             error:&err];
  CHECK(compiler, "newCompilerWithDescriptor: %s", err.description.UTF8String);
  NSMutableArray *render_psos = [NSMutableArray array], *compute_psos = [NSMutableArray array];
  int libs = 0, lib_fail = 0, vertex = 0, fragment = 0, kernel = 0;
  int dev_ok = 0, dev_fail = 0, mtl4c_ok = 0, mtl4c_fail = 0, frag_ok = 0, rasterless = 0;
  char first_error[512] = "";
  while (sqlite3_step(st) == SQLITE_ROW) {
    @autoreleasepool {
      const void *blob = sqlite3_column_blob(st, 0);
      int len = sqlite3_column_bytes(st, 0);
      dispatch_data_t data = dispatch_data_create(blob, len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
      id<MTLLibrary> lib = [m->device newLibraryWithData:data error:&err];
      dispatch_release(data);
      if (!lib) {
        lib_fail++;
        continue;
      }
      libs++;
      for (NSString *name in lib.functionNames) {
        id<MTLFunction> fn = [[lib newFunctionWithName:name] autorelease];
        MTL4LibraryFunctionDescriptor *fd = [[MTL4LibraryFunctionDescriptor new] autorelease];
        fd.name = name;
        fd.library = lib;
        NSError *e1 = nil, *e2 = nil;
        id dev = nil, m4 = nil;
        switch (fn.functionType) {
        case MTLFunctionTypeVertex: {
          vertex++;
          MTLRenderPipelineDescriptor *rd = [[MTLRenderPipelineDescriptor new] autorelease];
          rd.vertexFunction = fn;
          rd.fragmentFunction = constant_fragment;
          rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
          dev = [m->device newRenderPipelineStateWithDescriptor:rd error:&e1];
          MTL4RenderPipelineDescriptor *r4 = [[MTL4RenderPipelineDescriptor new] autorelease];
          r4.vertexFunctionDescriptor = fd;
          r4.fragmentFunctionDescriptor = constant_fd;
          r4.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
          m4 = [compiler newRenderPipelineStateWithDescriptor:r4 compilerTaskOptions:nil error:&e2];
          if (!dev && !m4) {
            /* a vertex function that returns nothing (stream output): DXMT builds it with
               rasterization off and no fragment function */
            [dev release];
            rd.fragmentFunction = nil;
            rd.rasterizationEnabled = NO;
            e1 = nil;
            dev = [m->device newRenderPipelineStateWithDescriptor:rd error:&e1];
            r4.fragmentFunctionDescriptor = nil;
            r4.rasterizationEnabled = NO;
            e2 = nil;
            m4 = [compiler newRenderPipelineStateWithDescriptor:r4 compilerTaskOptions:nil error:&e2];
            if (dev)
              rasterless++;
          }
          if (dev)
            [render_psos addObject:dev];
          break;
        }
        case MTLFunctionTypeKernel: {
          kernel++;
          dev = [m->device newComputePipelineStateWithFunction:fn error:&e1];
          MTL4ComputePipelineDescriptor *c4 = [[MTL4ComputePipelineDescriptor new] autorelease];
          c4.computeFunctionDescriptor = fd;
          m4 = [compiler newComputePipelineStateWithDescriptor:c4 compilerTaskOptions:nil error:&e2];
          if (dev)
            [compute_psos addObject:dev];
          break;
        }
        case MTLFunctionTypeFragment: {
          /* links only if it reads no varyings: counted, not judged */
          fragment++;
          MTLRenderPipelineDescriptor *rd = [[MTLRenderPipelineDescriptor new] autorelease];
          rd.vertexFunction = position_only;
          rd.fragmentFunction = fn;
          rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
          id f = [m->device newRenderPipelineStateWithDescriptor:rd error:&e1];
          if (f) {
            frag_ok++;
            [render_psos addObject:f];
            [f release];
          }
          continue;
        }
        default:
          continue;
        }
        dev ? dev_ok++ : dev_fail++;
        m4 ? mtl4c_ok++ : mtl4c_fail++;
        if ((!dev || !m4) && !first_error[0])
          snprintf(first_error, sizeof(first_error), "%s: device %s; MTL4Compiler %s", name.UTF8String,
                   e1 ? e1.localizedDescription.UTF8String : "ok", e2 ? e2.localizedDescription.UTF8String : "ok");
        [dev release];
        [m4 release];
      }
      [lib release];
    }
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  printf("probe: %d metallibs (%d failed to load): %d vertex, %d fragment, %d kernel functions\n", libs, lib_fail,
         vertex, fragment, kernel);
  printf("probe: device-built pipelines %d ok, %d failed; MTL4Compiler pipelines %d ok, %d failed (%d vertex "
         "functions without outputs, built with rasterization off); fragment-only links %d\n",
         dev_ok, dev_fail, mtl4c_ok, mtl4c_fail, rasterless, frag_ok);
  if (first_error[0])
    printf("probe: first failure: %s\n", first_error);
  CHECK(libs > 0 && !lib_fail, "airconv metallibs load");
  CHECK(dev_ok > 0 && !dev_fail, "device-built pipelines from airconv metallibs");

  /* bind every device-built pipeline on Metal 4 encoders through the replay, and commit; the
     pipelines are resident like any allocation (commit_and_wait commits the set) */
  for (id pso in render_psos)
    [m->residency addAllocation:pso];
  for (id pso in compute_psos)
    [m->residency addAllocation:pso];
  begin(m);
  struct WMTMtl4BindingState state;
  memset(&state, 0, sizeof(state));
  id<MTLTexture> target = resident_texture(m, MTLTextureUsageRenderTarget);
  struct WMTRenderPassInfo info;
  memset(&info, 0, sizeof(info));
  info.colors[0].texture = H(target);
  info.colors[0].load_action = WMTLoadActionClear;
  info.colors[0].store_action = WMTStoreActionStore;
  info.default_raster_sample_count = 1;
  info.render_target_width = 4;
  info.render_target_height = 4;
  obj_handle_t enc = render_encoder(m, &info, &state);
  for (id pso in render_psos) {
    struct chain c = {0};
    struct wmtcmd_render_setpso *setpso = chain_add(&c, sizeof(*setpso), WMTRenderCommandSetPSO);
    setpso->pso = H(pso);
    replay(_MTL4RenderCommandEncoder_encodeCommands, enc, &state, &c);
  }
  [(id<MTL4RenderCommandEncoder>)enc endEncoding];
  if (compute_psos.count) {
    enc = compute_encoder(m, &state);
    for (id pso in compute_psos) {
      struct chain c = {0};
      struct wmtcmd_compute_setpso *setpso = chain_add(&c, sizeof(*setpso), WMTComputeCommandSetPSO);
      setpso->pso = H(pso);
      setpso->threadgroup_size = (struct WMTSize){1, 1, 1};
      replay(_MTL4ComputeCommandEncoder_encodeCommands, enc, &state, &c);
    }
    [(id<MTL4ComputeCommandEncoder>)enc endEncoding];
  }
  struct WMTMtl4CommitFeedback fb;
  commit_and_wait(m, &fb, "probe bind");
  printf("probe: bound %lu render and %lu compute pipelines (device-built, airconv's AIR) on Metal 4 encoders: %s\n",
         (unsigned long)render_psos.count, (unsigned long)compute_psos.count, fb.error ? "GPU error" : "ok");
  for (id pso in render_psos)
    [m->residency removeAllocation:pso];
  for (id pso in compute_psos)
    [m->residency removeAllocation:pso];
  [m->residency commit];
  [compiler release];
  [constant_fragment release];
  [position_only release];
  [own release];
}

int
main(int argc, char **argv) {
  @autoreleasepool {
    const char *db = NULL;
    int limit = 500;
    for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--metallibs") && i + 1 < argc)
        db = argv[++i];
      else if (!strcmp(argv[i], "--limit") && i + 1 < argc)
        limit = atoi(argv[++i]);
    }
    struct mtl4 m = {0};
    m.device = MTLCreateSystemDefaultDevice();
    struct unixcall_generic_obj_uint64_ret support = {.handle = H(m.device)};
    _MTLDevice_metal4Support(&support);
    printf("device: %s, metal4Support=%llu (0 = supported)\n", m.device.name.UTF8String,
           (unsigned long long)support.ret);
    if (support.ret != WMTMetal4Supported) {
      printf("no Metal 4 here: nothing to test\n");
      return 1;
    }
    struct unixcall_generic_obj_obj_ret p = {.handle = H(m.device)};
    _MTLDevice_newMTL4CommandQueue(&p);
    m.queue = p.ret;
    _MTLDevice_newCommandAllocator(&p);
    m.allocator = p.ret;
    _MTLDevice_newMTL4CommandBuffer(&p);
    m.cmdbuf = p.ret;
    CHECK(m.queue && m.allocator && m.cmdbuf, "queue, allocator, command buffer");
    if (!m.queue || !m.allocator || !m.cmdbuf)
      return 1;
    MTLResidencySetDescriptor *rd = [[MTLResidencySetDescriptor new] autorelease];
    NSError *err = nil;
    m.residency = [m.device newResidencySetWithDescriptor:rd error:&err];
    struct unixcall_generic_obj_obj_noret add = {.handle = m.queue, .arg = H(m.residency)};
    _MTL4CommandQueue_addResidencySet(&add);

    test_replay(&m);
    /* part 3: the same with nothing made resident by hand: only what the replays put in the chunk's set */
    m.chunk = [m.device newResidencySetWithDescriptor:rd error:&err];
    test_replay(&m);
    [m.chunk release];
    m.chunk = nil;
    test_cross_commit_fence(&m);
    test_drawable(&m);
    if (db)
      probe_metallibs(&m, db, limit);

    _MTL4CommandQueue_removeResidencySet(&add);
    [(id)m.cmdbuf release];
    [(id)m.allocator release];
    [(id)m.queue release];
    [m.residency release];
  }
  printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
