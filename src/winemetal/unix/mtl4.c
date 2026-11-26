/*
 * gw2 (#32): the Metal 4 bridge (DXMT_METAL4), unix side (call numbers: winemetal_calls.h);
 * the PE side is winemetal_thunks.c, the declarations and the contract are in winemetal.h.
 *
 * DXMT's encode thread keeps building today's wmtcmd_* packets. The three replays below run them on
 * Metal 4 encoders: Metal 4 binds through argument tables (setAddress/setTexture, snapshotted at each
 * draw or dispatch), has no useResource (residency sets), no setBytes and no blit encoder, takes GPU
 * addresses for index and indirect buffers, and orders work with fences and barriers on MTLStages.
 * The template is d9mt's MTL4 bridge (bridge/unix/winemetal_unix.c), which runs under Rosetta on this
 * Mac.
 */
#include <os/lock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "airconv_public.h"
#include "mtl4.h"

#define STATUS_SUCCESS 0

NTSTATUS
_MTLDevice_metal4Support(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  if (@available(macOS 26.0, *)) {
    id<MTLDevice> device = (id<MTLDevice>)params->handle;
    params->ret = [device supportsFamily:MTLGPUFamilyMetal4] ? WMTMetal4Supported : WMTMetal4NoFamily;
  } else {
    params->ret = WMTMetal4NoOS;
  }
  return STATUS_SUCCESS;
}

NTSTATUS
_MTLDevice_newMTL4CommandQueue(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = 0;
  if (@available(macOS 26.0, *))
    params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newMTL4CommandQueue];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTLDevice_newCommandAllocator(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = 0;
  if (@available(macOS 26.0, *))
    params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandAllocator];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTLDevice_newMTL4CommandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = 0;
  if (@available(macOS 26.0, *))
    params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandBuffer];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTLDevice_newArgumentTable(void *obj) {
  struct unixcall_mtl4_new_argtable *params = obj;
  const struct WMTArgTableInfo *info = params->info.ptr;
  params->ret = 0;
  params->ret_error = 0;
  if (@available(macOS 26.0, *)) {
    MTL4ArgumentTableDescriptor *desc = [[MTL4ArgumentTableDescriptor alloc] init];
    desc.maxBufferBindCount = info->max_buffers;
    desc.maxTextureBindCount = info->max_textures;
    desc.maxSamplerStateBindCount = info->max_samplers;
    desc.initializeBindings = info->initialize_bindings ? YES : NO;
    desc.supportAttributeStrides = info->support_attribute_strides ? YES : NO;
    NSError *err = nil;
    params->ret = (obj_handle_t)[(id<MTLDevice>)params->device newArgumentTableWithDescriptor:desc error:&err];
    params->ret_error = (obj_handle_t)[err retain];
    [desc release];
  }
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandAllocator_reset(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  id<MTL4CommandAllocator> allocator = (id<MTL4CommandAllocator>)params->handle;
  params->ret = [allocator allocatedSize];
  [allocator reset];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandBuffer_beginCommandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandBuffer>)params->handle beginCommandBufferWithAllocator:(id<MTL4CommandAllocator>)params->arg];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandBuffer_endCommandBuffer(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTL4CommandBuffer>)params->handle endCommandBuffer];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandBuffer_useResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandBuffer>)params->handle useResidencySet:(id<MTLResidencySet>)params->arg];
  return STATUS_SUCCESS;
}

static os_unfair_lock g_event_lock = OS_UNFAIR_LOCK_INIT;

/* gw2 (#242): DXMT_MTL4_PRESENT=done. The drawable's present once its pass's commit has completed (the
   feedback handler) and signalDrawable has been called (MTL4CommandQueue_commitPresent): whichever of
   the two arrives second presents, exactly once, and frees the gate. Apple's reference says a signal
   after the present fails, so the order isn't left to timing. */
struct present_gate {
  _Atomic uint32_t arrived;   /* the handler and the call, one each */
  _Atomic uint32_t completed; /* the handler has run (present_early) */
  id<MTLDrawable> drawable;   /* retained until presented */
  uint64_t mode;
  double value; /* the minimum duration, or the deadline's time; set by the call before it arrives */
  struct WMTMtl4CommitFeedback *feedback;
};

/* value: the minimum duration, or the deadline's time (CACurrentMediaTime's clock) */
static void
present_drawable(id<MTLDrawable> drawable, uint64_t mode, double value, struct WMTMtl4CommitFeedback *feedback,
                 bool early) {
  feedback->present_early = early;
  feedback->present_time = CACurrentMediaTime();
  switch ((enum WMTDrawablePresentMode)mode) {
  case WMTDrawablePresentAfterMinimumDuration:
    [drawable presentAfterMinimumDuration:value];
    break;
  case WMTDrawablePresentWithDeadline:
    [drawable presentAtTime:value];
    break;
  default:
    [drawable present];
    break;
  }
}

static void
present_gate_present(struct present_gate *gate) {
  bool early = !atomic_load_explicit(&gate->completed, memory_order_acquire);
  present_drawable(gate->drawable, gate->mode, gate->value, gate->feedback, early);
  [gate->drawable release];
  free(gate);
}

static void
present_gate_arrive(struct present_gate *gate) {
  if (atomic_fetch_add_explicit(&gate->arrived, 1, memory_order_acq_rel) == 1)
    present_gate_present(gate); /* the other one came first */
}

/* one commit with feedback; gate (gw2 #242), if any, arrives from the handler before done is set */
static void
commit_with_feedback(
    id<MTL4CommandQueue> queue, const id<MTL4CommandBuffer> *cmdbufs, uint64_t count,
    struct WMTMtl4CommitFeedback *feedback, id<MTLSharedEvent> event, uint64_t value, struct present_gate *gate
) {
  /* one options object per commit: a reused one runs its handlers for its first commit only
     (measured by d9mt on macOS 27). The block (copied by Metal) retains the event. */
  MTL4CommitOptions *options = [[MTL4CommitOptions alloc] init];
  [options addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
    feedback->gpu_start = fb.GPUStartTime;
    feedback->gpu_end = fb.GPUEndTime;
    feedback->error = (obj_handle_t)[fb.error retain];
    if (gate) {
      atomic_store_explicit(&gate->completed, 1, memory_order_release);
      present_gate_arrive(gate);
    }
    atomic_store_explicit((_Atomic uint64_t *)&feedback->done, 1, memory_order_release);
    if (event) {
      /* only forward: should handlers of two commits run at once, a waiter for the later value
         must not see it lowered by the earlier one */
      os_unfair_lock_lock(&g_event_lock);
      if (event.signaledValue < value)
        event.signaledValue = value;
      os_unfair_lock_unlock(&g_event_lock);
    }
  }];
  [queue commit:cmdbufs count:count options:options];
  [options release];
}

NTSTATUS
_MTL4CommandQueue_commit(void *obj) {
  struct unixcall_mtl4_queue_commit *params = obj;
  id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)params->queue;
  const id<MTL4CommandBuffer> *cmdbufs = (const id<MTL4CommandBuffer> *)params->cmdbufs.ptr;
  struct WMTMtl4CommitFeedback *feedback = params->feedback.ptr;
  if (!feedback) {
    [queue commit:cmdbufs count:params->count];
    return STATUS_SUCCESS;
  }
  commit_with_feedback(
      queue, cmdbufs, params->count, feedback, (id<MTLSharedEvent>)params->event, params->event_value, NULL
  );
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandQueue_commitPresent(void *obj) {
  struct unixcall_mtl4_queue_commit_present *params = obj;
  id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)params->queue;
  id<MTL4CommandBuffer> cmdbuf = (id<MTL4CommandBuffer>)params->cmdbuf;
  id<MTLDrawable> drawable = (id<MTLDrawable>)params->drawable;
  struct WMTMtl4CommitFeedback *feedback = params->feedback.ptr;
  struct present_gate *gate = calloc(1, sizeof(*gate));
  if (gate) {
    gate->drawable = [drawable retain];
    gate->mode = params->mode;
    gate->feedback = feedback;
  }
  /* without a gate (no memory): today's order, the present right after the signal (below) */
  commit_with_feedback(
      queue, &cmdbuf, 1, feedback, (id<MTLSharedEvent>)params->event, params->event_value, gate
  );
  [queue signalDrawable:drawable];
  double now = CACurrentMediaTime();
  feedback->present_encode = now;
  double value = params->mode == WMTDrawablePresentWithDeadline ? now + params->value : params->value;
  if (!gate) {
    bool early = !atomic_load_explicit((_Atomic uint64_t *)&feedback->done, memory_order_acquire);
    present_drawable(drawable, params->mode, value, feedback, early);
    return STATUS_SUCCESS;
  }
  gate->value = value;
  present_gate_arrive(gate);
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandQueue_waitForDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle waitForDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandQueue_signalDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle signalDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandQueue_signalEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle signalEvent:(id<MTLEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandQueue_waitForEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle waitForEvent:(id<MTLEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandQueue_addResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle addResidencySet:(id<MTLResidencySet>)params->arg];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandQueue_removeResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTL4CommandQueue>)params->handle removeResidencySet:(id<MTLResidencySet>)params->arg];
  return STATUS_SUCCESS;
}

NTSTATUS
_MetalLayer_residencySet(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = 0;
  if (@available(macOS 26.0, *))
    params->ret = (obj_handle_t)[(CAMetalLayer *)params->handle residencySet];
  return STATUS_SUCCESS;
}

NTSTATUS
_MetalDrawable_present(void *obj) {
  struct unixcall_mtl4_drawable_present *params = obj;
  id<MTLDrawable> drawable = (id<MTLDrawable>)params->drawable;
  switch ((enum WMTDrawablePresentMode)params->mode) {
  case WMTDrawablePresentAfterMinimumDuration:
    [drawable presentAfterMinimumDuration:params->value];
    break;
  case WMTDrawablePresentWithDeadline:
    [drawable presentAtTime:CACurrentMediaTime() + params->value];
    break;
  default:
    [drawable present];
    break;
  }
  return STATUS_SUCCESS;
}

static void resident_attachments(struct WMTMtl4BindingState *state, const struct WMTRenderPassInfo *info);

static void
bind_tables(id<MTL4RenderCommandEncoder> encoder, const struct WMTMtl4BindingState *state) {
  static const MTLRenderStages stages[WMT_MTL4_TABLE_STAGES] = {
      MTLRenderStageVertex, MTLRenderStageFragment, MTLRenderStageObject, MTLRenderStageMesh
  };
  for (unsigned i = 0; i < WMT_MTL4_TABLE_STAGES; i++)
    if (state->tables[i])
      [encoder setArgumentTable:(id<MTL4ArgumentTable>)state->tables[i] atStages:stages[i]];
}

NTSTATUS
_MTL4CommandBuffer_renderCommandEncoder(void *obj) {
  struct unixcall_mtl4_new_encoder *params = obj;
  const struct WMTRenderPassInfo *info = params->info.ptr;
  struct WMTMtl4BindingState *state = params->state.ptr;
  MTL4RenderPassDescriptor *descriptor = [[MTL4RenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    if (!info->colors[i].texture)
      continue;
    MTLRenderPassColorAttachmentDescriptor *color = descriptor.colorAttachments[i];
    color.clearColor = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
        info->colors[i].clear_color.a
    );
    color.level = info->colors[i].level;
    color.slice = info->colors[i].slice;
    color.depthPlane = info->colors[i].depth_plane;
    color.texture = (id<MTLTexture>)info->colors[i].texture;
    color.loadAction = (MTLLoadAction)info->colors[i].load_action;
    color.storeAction = (MTLStoreAction)info->colors[i].store_action;
    color.resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
    color.resolveLevel = info->colors[i].resolve_level;
    color.resolveSlice = info->colors[i].resolve_slice;
    color.resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }
  if (info->depth.texture) {
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }
  if (info->stencil.texture) {
    descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
    descriptor.stencilAttachment.level = info->stencil.level;
    descriptor.stencilAttachment.slice = info->stencil.slice;
    descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
    descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
    descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
  }
  descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
  descriptor.renderTargetArrayLength = info->render_target_array_length;
  descriptor.renderTargetHeight = info->render_target_height;
  descriptor.renderTargetWidth = info->render_target_width;
  /* the Metal 3 default, MTLVisibilityResultTypeReset: each pass writes its own slots */
  descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;
  if (info->tile_height && info->tile_width) {
    descriptor.tileWidth = info->tile_width;
    descriptor.tileHeight = info->tile_height;
  }
  id<MTL4RenderCommandEncoder> encoder =
      [(id<MTL4CommandBuffer>)params->cmdbuf renderCommandEncoderWithDescriptor:descriptor];
  [descriptor release];
  if (encoder && state) {
    bind_tables(encoder, state);
    resident_attachments(state, info);
  }
  params->ret = (obj_handle_t)encoder;
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandBuffer_computeCommandEncoder(void *obj) {
  struct unixcall_mtl4_new_encoder *params = obj;
  struct WMTMtl4BindingState *state = params->state.ptr;
  id<MTL4ComputeCommandEncoder> encoder = [(id<MTL4CommandBuffer>)params->cmdbuf computeCommandEncoder];
  if (state)
    state->threadgroup_size = (struct WMTSize){0, 0, 0}; /* no pipeline yet */
  if (encoder && state && state->tables[WMTMtl4TableCompute])
    [encoder setArgumentTable:(id<MTL4ArgumentTable>)state->tables[WMTMtl4TableCompute]];
  params->ret = (obj_handle_t)encoder;
  return STATUS_SUCCESS;
}

/* --- binding into the argument tables */

/* gw2 (#32 part 3): residency. Metal 4 makes nothing resident by itself, not even what an encoder
   names directly, so every allocation a packet names goes into the state's residency set (the
   chunk's). A small direct-mapped cache skips repeats within one call; the set ignores the rest.
   gw2 (#153): except what the device's set (on the queue) already holds: a packet with
   WMT_CMD_RESIDENT (its pipeline, buffers, index buffer), the bytes buffer with
   WMT_MTL4_BYTES_RESIDENT. */
struct replay {
  struct WMTMtl4BindingState *state;
  obj_handle_t recent[64];
};

static inline bool
residency_seen(struct replay *r, obj_handle_t handle) {
  unsigned slot = (unsigned)((handle >> 4) ^ (handle >> 10)) & 63;
  if (r->recent[slot] == handle)
    return true;
  r->recent[slot] = handle;
  return false;
}

static inline void
residency_add(struct replay *r, id<MTLAllocation> allocation) {
  [(id<MTLResidencySet>)r->state->residency_set addAllocation:allocation];
  r->state->residency_adds++;
}

/* a pipeline state, an indirect command buffer: an allocation of its own */
static inline void
resident_object(struct replay *r, obj_handle_t handle) {
  if (!handle || !r->state->residency_set || residency_seen(r, handle))
    return;
  residency_add(r, (id<MTLAllocation>)handle);
}

/* a buffer: itself, or its heap if it was placed in one (after the cache check) */
static inline void
add_buffer(struct replay *r, id<MTLBuffer> buffer) {
  id<MTLHeap> heap = buffer.heap;
  residency_add(r, heap ? (id<MTLAllocation>)heap : (id<MTLAllocation>)buffer);
}

/* a texture: a view's parent (the memory), a buffer-backed texture's buffer, or its heap (after the
   cache check) */
static inline void
add_texture(struct replay *r, id<MTLTexture> texture) {
  while (texture.parentTexture)
    texture = texture.parentTexture;
  if (texture.buffer) {
    add_buffer(r, texture.buffer);
    return;
  }
  id<MTLHeap> heap = texture.heap;
  residency_add(r, heap ? (id<MTLAllocation>)heap : (id<MTLAllocation>)texture);
}

static inline void
resident_buffer(struct replay *r, obj_handle_t handle) {
  if (!handle || !r->state->residency_set || residency_seen(r, handle))
    return;
  add_buffer(r, (id<MTLBuffer>)handle);
}

static inline void
resident_texture(struct replay *r, obj_handle_t handle) {
  if (!handle || !r->state->residency_set || residency_seen(r, handle))
    return;
  add_texture(r, (id<MTLTexture>)handle);
}

/* UseResource: a buffer or a texture (the packets don't say which) */
static inline void
resident_resource(struct replay *r, obj_handle_t handle) {
  if (!handle)
    return;
  if (!r->state->residency_set) {
    r->state->use_resource_dropped++;
    return;
  }
  if (residency_seen(r, handle))
    return;
  if ([(id<MTLResource>)handle conformsToProtocol:@protocol(MTLTexture)])
    add_texture(r, (id<MTLTexture>)handle);
  else
    add_buffer(r, (id<MTLBuffer>)handle);
}

/* a one-entry cache of a buffer's GPU address and length: DXMT binds its per-encoder argument
   buffer at several indices, and draws reuse their index buffer. A miss makes the buffer resident,
   unless the packet said it is (gw2 #153). */
struct buffer_cache {
  obj_handle_t buffer;
  uint64_t address;
  uint64_t length;
};

static inline bool
packet_resident(const struct wmtcmd_base *cmd) {
  return cmd->flags & WMT_CMD_RESIDENT;
}

static inline void
buffer_lookup(struct replay *r, struct buffer_cache *cache, obj_handle_t buffer, bool resident) {
  if (cache->buffer == buffer)
    return;
  cache->buffer = buffer;
  cache->address = buffer ? [(id<MTLBuffer>)buffer gpuAddress] : 0;
  cache->length = buffer ? [(id<MTLBuffer>)buffer length] : 0;
  if (!resident)
    resident_buffer(r, buffer);
}

/* gw2 (#153): a resident indexed draw carries its index buffer's address and length; Metal isn't
   asked. A cache entry from it is right for any later packet: residency is the buffer's own. */
static inline void
index_lookup(
    struct replay *r, struct buffer_cache *cache, obj_handle_t buffer, bool resident, uint64_t address,
    uint64_t length
) {
  if (resident && buffer) {
    cache->buffer = buffer;
    cache->address = address;
    cache->length = length;
    return;
  }
  buffer_lookup(r, cache, buffer, false);
}

static inline void
set_address(struct WMTMtl4BindingState *state, unsigned stage, unsigned index, uint64_t address) {
  id<MTL4ArgumentTable> table = (id<MTL4ArgumentTable>)state->tables[stage];
  if (!table || index >= WMT_MTL4_MAX_BUFFERS) {
    state->unsupported++;
    return;
  }
  [table setAddress:address atIndex:index];
}

/* Set*Buffer: remember the buffer's address for later offsets at the index, and bind */
static inline void
set_buffer(
    struct replay *r, struct buffer_cache *cache, unsigned stage, obj_handle_t buffer, uint64_t offset,
    unsigned index, bool resident
) {
  buffer_lookup(r, cache, buffer, resident);
  if (index < WMT_MTL4_MAX_BUFFERS)
    r->state->buffer_base[stage][index] = cache->address;
  set_address(r->state, stage, index, cache->address ? cache->address + offset : 0);
}

/* Set*BufferOffset: the buffer the last Set*Buffer bound at the index, at a new offset */
static inline void
set_buffer_offset(struct WMTMtl4BindingState *state, unsigned stage, uint64_t offset, unsigned index) {
  uint64_t base = index < WMT_MTL4_MAX_BUFFERS ? state->buffer_base[stage][index] : 0;
  set_address(state, stage, index, base ? base + offset : 0);
}

/* Set*Bytes: a copy into the bytes buffer (256-byte aligned, as constant buffers want), bound.
   False: it doesn't fit; the caller stops the replay here (resume) for a new bytes buffer. */
static bool
set_bytes(struct replay *r, unsigned stage, const void *bytes, uint64_t length, unsigned index) {
  struct WMTMtl4BindingState *state = r->state;
  if (!state->bytes_buffer) {
    state->bytes_overflow++;
    return true; /* skipped: nothing to resume into */
  }
  uint64_t offset = (state->bytes_offset + 255) & ~255ull;
  if (offset + length > state->bytes_end) {
    state->bytes_needed = length + 256;
    return false;
  }
  id<MTLBuffer> buffer = (id<MTLBuffer>)state->bytes_buffer;
  memcpy((char *)[buffer contents] + offset, bytes, length);
  state->bytes_offset = offset + length;
  if (!(state->flags & WMT_MTL4_BYTES_RESIDENT)) /* gw2 (#153) */
    resident_buffer(r, state->bytes_buffer);
  if (index < WMT_MTL4_MAX_BUFFERS)
    state->buffer_base[stage][index] = 0; /* an offset at this index has no buffer to move in */
  set_address(state, stage, index, [buffer gpuAddress] + offset);
  return true;
}

static inline void
set_texture(struct replay *r, unsigned stage, obj_handle_t texture, unsigned index) {
  id<MTL4ArgumentTable> table = (id<MTL4ArgumentTable>)r->state->tables[stage];
  if (!table) {
    r->state->unsupported++;
    return;
  }
  MTLResourceID rid = {0};
  if (texture)
    rid = [(id<MTLTexture>)texture gpuResourceID];
  resident_texture(r, texture);
  [table setTexture:rid atIndex:index];
}

static inline uint64_t
buffer_address(struct replay *r, struct buffer_cache *cache, obj_handle_t buffer, uint64_t offset, bool resident) {
  buffer_lookup(r, cache, buffer, resident);
  return cache->address + offset;
}

/* a render pass's attachments and visibility buffer (the caller's state is its own memory, written
   by this call as by the replays) */
static void
resident_attachments(struct WMTMtl4BindingState *state, const struct WMTRenderPassInfo *info) {
  if (!state->residency_set)
    return;
  struct replay replay = {.state = state}, *r = &replay;
  if (!(state->flags & WMT_MTL4_NO_ATTACHMENT_RESIDENCY)) {
    for (unsigned i = 0; i < 8; i++) {
      resident_texture(r, info->colors[i].texture);
      resident_texture(r, info->colors[i].resolve_texture);
    }
    resident_texture(r, info->depth.texture);
    resident_texture(r, info->stencil.texture);
  }
  resident_buffer(r, info->visibility_buffer);
}

/* the compute pipeline's threadgroup size, kept in the state across replay calls */
static inline MTLSize
state_threadgroup(const struct WMTMtl4BindingState *state) {
  return MTLSizeMake(state->threadgroup_size.width, state->threadgroup_size.height, state->threadgroup_size.depth);
}

/* --- the replays */

NTSTATUS
_MTL4RenderCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_mtl4_encode *params = obj;
  struct WMTMtl4BindingState *state = params->state.ptr;
  state->resume.ptr = NULL;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  struct replay replay = {.state = state}, *r = &replay;
  id<MTL4RenderCommandEncoder> encoder = (id<MTL4RenderCommandEncoder>)params->encoder;
  struct buffer_cache bound = {0}, index = {0}, indirect = {0};
  enum { V = WMTMtl4TableVertex, F = WMTMtl4TableFragment, O = WMTMtl4TableObject, M = WMTMtl4TableMesh };
  while (next) {
    switch ((enum WMTRenderCommandType)next->type) {
    default:
      state->unsupported++;
      break;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource:
      resident_resource(r, ((struct wmtcmd_render_useresource *)next)->resource);
      break;
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      set_buffer(r, &bound, V, body->buffer, body->offset, body->index, packet_resident(next));
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      set_buffer_offset(state, V, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      set_buffer(r, &bound, F, body->buffer, body->offset, body->index, packet_resident(next));
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      set_buffer_offset(state, F, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      set_buffer(r, &bound, M, body->buffer, body->offset, body->index, packet_resident(next));
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      set_buffer_offset(state, M, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      set_buffer(r, &bound, O, body->buffer, body->offset, body->index, packet_resident(next));
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      set_buffer_offset(state, O, body->offset, body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      if (!set_bytes(r, F, body->bytes.ptr, body->length, body->index)) {
        state->resume.ptr = (void *)next;
        return STATUS_SUCCESS;
      }
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      set_texture(r, F, body->texture, body->index);
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      struct wmtcmd_render_setrasterizerstate *body = (struct wmtcmd_render_setrasterizerstate *)next;
      [encoder setTriangleFillMode:(MTLTriangleFillMode)body->fill_mode];
      [encoder setCullMode:(MTLCullMode)body->cull_mode];
      [encoder setDepthClipMode:(MTLDepthClipMode)body->depth_clip_mode];
      [encoder setDepthBias:body->depth_bias slopeScale:body->scole_scale clamp:body->depth_bias_clamp];
      [encoder setFrontFacingWinding:(MTLWinding)body->winding];
      break;
    }
    case WMTRenderCommandSetViewports: {
      struct wmtcmd_render_setviewports *body = (struct wmtcmd_render_setviewports *)next;
      [encoder setViewports:(const MTLViewport *)body->viewports.ptr count:body->viewport_count];
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      struct wmtcmd_render_setscissorrects *body = (struct wmtcmd_render_setscissorrects *)next;
      [encoder setScissorRects:(const MTLScissorRect *)body->scissor_rects.ptr count:body->rect_count];
      break;
    }
    case WMTRenderCommandSetPSO: {
      struct wmtcmd_render_setpso *body = (struct wmtcmd_render_setpso *)next;
      if (!packet_resident(next))
        resident_object(r, body->pso);
      [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetDepthStencilState: {
      struct wmtcmd_render_setdepthstencilstate *body = (struct wmtcmd_render_setdepthstencilstate *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->depth_stencil_state];
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetBlendFactor: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      break;
    }
    case WMTRenderCommandSetStencilRef: {
      struct wmtcmd_render_setstencilref *body = (struct wmtcmd_render_setstencilref *)next;
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      struct wmtcmd_render_setvisibilitymode *body = (struct wmtcmd_render_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode offset:body->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      struct wmtcmd_render_draw *body = (struct wmtcmd_render_draw *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  vertexStart:body->vertex_start
                  vertexCount:body->vertex_count
                instanceCount:body->instance_count
                 baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      struct wmtcmd_render_draw_indexed *body = (struct wmtcmd_render_draw_indexed *)next;
      index_lookup(
          r, &index, body->index_buffer, packet_resident(next), body->index_buffer_address, body->index_buffer_length
      );
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                          indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:index.address + body->index_buffer_offset
                   indexBufferLength:index.length - body->index_buffer_offset
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
               indirectBuffer:buffer_address(
                                  r, &indirect, body->indirect_args_buffer, body->indirect_args_offset, false
                              )];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      bool resident = packet_resident(next);
      index_lookup(r, &index, body->index_buffer, resident, body->index_buffer_address, body->index_buffer_length);
      [encoder
          drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                      indexType:(MTLIndexType)body->index_type
                    indexBuffer:index.address + body->index_buffer_offset
              indexBufferLength:index.length - body->index_buffer_offset
                 indirectBuffer:buffer_address(
                                    r, &indirect, body->indirect_args_buffer, body->indirect_args_offset, resident
                                )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      struct wmtcmd_render_draw_meshthreadgroups *body = (struct wmtcmd_render_draw_meshthreadgroups *)next;
      [encoder drawMeshThreadgroups:MTLSizeMake(
                                        body->threadgroup_per_grid.width, body->threadgroup_per_grid.height,
                                        body->threadgroup_per_grid.depth
                                    )
          threadsPerObjectThreadgroup:MTLSizeMake(
                                          body->object_threadgroup_size.width, body->object_threadgroup_size.height,
                                          body->object_threadgroup_size.depth
                                      )
            threadsPerMeshThreadgroup:MTLSizeMake(
                                          body->mesh_threadgroup_size.width, body->mesh_threadgroup_size.height,
                                          body->mesh_threadgroup_size.depth
                                      )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      struct wmtcmd_render_draw_meshthreadgroups_indirect *body =
          (struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      [encoder
          drawMeshThreadgroupsWithIndirectBuffer:buffer_address(
                                                     r, &indirect, body->indirect_args_buffer,
                                                     body->indirect_args_offset, false
                                                 )
                     threadsPerObjectThreadgroup:MTLSizeMake(
                                                     body->object_threadgroup_size.width,
                                                     body->object_threadgroup_size.height,
                                                     body->object_threadgroup_size.depth
                                                 )
                       threadsPerMeshThreadgroup:MTLSizeMake(
                                                     body->mesh_threadgroup_size.width,
                                                     body->mesh_threadgroup_size.height,
                                                     body->mesh_threadgroup_size.depth
                                                 )];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      /* Metal 3's memoryBarrierWithScope:afterStages:beforeStages:; a Metal 4 barrier covers all
         memory, and the render stage bits are MTLStages' */
      struct wmtcmd_render_memory_barrier *body = (struct wmtcmd_render_memory_barrier *)next;
      [encoder barrierAfterEncoderStages:(MTLStages)body->stages_after
                     beforeEncoderStages:(MTLStages)body->stages_before
                       visibilityOptions:MTL4VisibilityOptionDevice];
      break;
    }
    case WMTRenderCommandDXMTGeometryDraw: {
      struct wmtcmd_render_dxmt_geometry_draw *body = (struct wmtcmd_render_dxmt_geometry_draw *)next;
      set_buffer_offset(state, O, body->draw_arguments_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexed: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed *body = (struct wmtcmd_render_dxmt_geometry_draw_indexed *)next;
      set_buffer(r, &bound, O, body->index_buffer, body->index_buffer_offset, SM50_BINDING_INDEX_INDEX_BUFFER, false);
      set_buffer_offset(state, O, body->draw_arguments_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indirect *body = (struct wmtcmd_render_dxmt_geometry_draw_indirect *)next;
      set_buffer(
          r, &bound, O, body->indirect_args_buffer, body->indirect_args_offset,
          SM50_BINDING_INDEX_INDIRECT_ARGUMENTS, false
      );
      [encoder drawMeshThreadgroupsWithIndirectBuffer:buffer_address(
                                                          r, &indirect, body->dispatch_args_buffer,
                                                          body->dispatch_args_offset, false
                                                      )
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      set_buffer(r, &bound, O, body->imm_draw_arguments, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS, false);
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)next;
      set_buffer(r, &bound, O, body->index_buffer, body->index_buffer_offset, SM50_BINDING_INDEX_INDEX_BUFFER, false);
      set_buffer(
          r, &bound, O, body->indirect_args_buffer, body->indirect_args_offset,
          SM50_BINDING_INDEX_INDIRECT_ARGUMENTS, false
      );
      [encoder drawMeshThreadgroupsWithIndirectBuffer:buffer_address(
                                                          r, &indirect, body->dispatch_args_buffer,
                                                          body->dispatch_args_offset, false
                                                      )
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      set_buffer(r, &bound, O, body->imm_draw_arguments, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS, false);
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDraw: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw *)next;
      set_buffer_offset(state, O, body->draw_arguments_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *)next;
      set_buffer(r, &bound, O, body->index_buffer, body->index_buffer_offset, SM50_BINDING_INDEX_INDEX_BUFFER, false);
      set_buffer_offset(state, O, body->draw_arguments_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)next;
      set_buffer(
          r, &bound, O, body->indirect_args_buffer, body->indirect_args_offset,
          SM50_BINDING_INDEX_INDIRECT_ARGUMENTS, false
      );
      [encoder drawMeshThreadgroupsWithIndirectBuffer:buffer_address(
                                                          r, &indirect, body->dispatch_args_buffer,
                                                          body->dispatch_args_offset, false
                                                      )
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      set_buffer(r, &bound, O, body->imm_draw_arguments, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS, false);
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)next;
      set_buffer(r, &bound, O, body->index_buffer, body->index_buffer_offset, SM50_BINDING_INDEX_INDEX_BUFFER, false);
      set_buffer(
          r, &bound, O, body->indirect_args_buffer, body->indirect_args_offset,
          SM50_BINDING_INDEX_INDIRECT_ARGUMENTS, false
      );
      [encoder drawMeshThreadgroupsWithIndirectBuffer:buffer_address(
                                                          r, &indirect, body->dispatch_args_buffer,
                                                          body->dispatch_args_offset, false
                                                      )
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      set_buffer(r, &bound, O, body->imm_draw_arguments, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS, false);
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:(MTLStages)body->stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:(MTLStages)body->stages];
      break;
    }
    case WMTRenderCommandSetViewport: {
      struct wmtcmd_render_setviewport *body = (struct wmtcmd_render_setviewport *)next;
      union {
        struct WMTViewport src;
        MTLViewport dst;
      } u = {.src = body->viewport};
      [encoder setViewport:u.dst];
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      struct wmtcmd_render_setscissorrect *body = (struct wmtcmd_render_setscissorrect *)next;
      union {
        struct WMTScissorRect src;
        MTLScissorRect dst;
      } u = {.src = body->scissor_rect};
      [encoder setScissorRect:u.dst];
      break;
    }
    case WMTRenderCommandDispatchThreadsPerTile: {
      struct wmtcmd_render_dispatch_threads_per_tile *body = (struct wmtcmd_render_dispatch_threads_per_tile *)next;
      [encoder dispatchThreadsPerTile:MTLSizeMake(body->width, body->height, 1)];
      break;
    }
    case WMTRenderCommandExecuteCommandsInBuffer: {
      struct wmtcmd_render_executecommands *body = (struct wmtcmd_render_executecommands *)next;
      resident_object(r, body->indirect_command_buffer);
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    }
    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4ComputeCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_mtl4_encode *params = obj;
  struct WMTMtl4BindingState *state = params->state.ptr;
  state->resume.ptr = NULL;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  struct replay replay = {.state = state}, *r = &replay;
  id<MTL4ComputeCommandEncoder> encoder = (id<MTL4ComputeCommandEncoder>)params->encoder;
  struct buffer_cache bound = {0}, indirect = {0};
  enum { C = WMTMtl4TableCompute };
  /* a pipeline's threadgroup size holds until the next pipeline, across calls (state_threadgroup) */
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    default:
      state->unsupported++;
      break;
    case WMTComputeCommandNop:
      break;
    case WMTComputeCommandDispatch: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:state_threadgroup(state)];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:state_threadgroup(state)];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      [encoder dispatchThreadgroupsWithIndirectBuffer:buffer_address(
                                                          r, &indirect, body->indirect_args_buffer,
                                                          body->indirect_args_offset, false
                                                      )
                                threadsPerThreadgroup:state_threadgroup(state)];
      break;
    }
    case WMTComputeCommandSetPSO: {
      struct wmtcmd_compute_setpso *body = (struct wmtcmd_compute_setpso *)next;
      if (!packet_resident(next))
        resident_object(r, body->pso);
      [encoder setComputePipelineState:(id<MTLComputePipelineState>)body->pso];
      state->threadgroup_size = body->threadgroup_size;
      break;
    }
    case WMTComputeCommandSetBuffer: {
      struct wmtcmd_compute_setbuffer *body = (struct wmtcmd_compute_setbuffer *)next;
      set_buffer(r, &bound, C, body->buffer, body->offset, body->index, packet_resident(next));
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      struct wmtcmd_compute_setbufferoffset *body = (struct wmtcmd_compute_setbufferoffset *)next;
      set_buffer_offset(state, C, body->offset, body->index);
      break;
    }
    case WMTComputeCommandUseResource:
      resident_resource(r, ((struct wmtcmd_compute_useresource *)next)->resource);
      break;
    case WMTComputeCommandSetBytes: {
      struct wmtcmd_compute_setbytes *body = (struct wmtcmd_compute_setbytes *)next;
      if (!set_bytes(r, C, body->bytes.ptr, body->length, body->index)) {
        state->resume.ptr = (void *)next;
        return STATUS_SUCCESS;
      }
      break;
    }
    case WMTComputeCommandSetTexture: {
      struct wmtcmd_compute_settexture *body = (struct wmtcmd_compute_settexture *)next;
      set_texture(r, C, body->texture, body->index);
      break;
    }
    case WMTComputeCommandUpdateFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageDispatch];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageDispatch];
      break;
    }
    case WMTComputeCommandMemoryBarrier:
      [encoder barrierAfterEncoderStages:MTLStageDispatch
                     beforeEncoderStages:MTLStageDispatch
                       visibilityOptions:MTL4VisibilityOptionDevice];
      break;
    case WMTComputeCommandExecuteCommandsInBuffer: {
      struct wmtcmd_compute_executecommands *body = (struct wmtcmd_compute_executecommands *)next;
      resident_object(r, body->indirect_command_buffer);
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    }
    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4ComputeCommandEncoder_encodeBlitCommands(void *obj) {
  struct unixcall_mtl4_encode *params = obj;
  struct WMTMtl4BindingState *state = params->state.ptr;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  struct replay replay = {.state = state}, *r = &replay;
  id<MTL4ComputeCommandEncoder> encoder = (id<MTL4ComputeCommandEncoder>)params->encoder;
  bool copied = false; /* a copy of this call is before the next one: order them */
#define BLIT_ORDER()                                                                                                   \
  do {                                                                                                                 \
    if (copied) {                                                                                                      \
      [encoder barrierAfterEncoderStages:MTLStageBlit                                                                  \
                     beforeEncoderStages:MTLStageBlit                                                                  \
                       visibilityOptions:MTL4VisibilityOptionDevice];                                                  \
      state->blit_barriers++;                                                                                          \
    }                                                                                                                  \
    copied = true;                                                                                                     \
  } while (0)
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      state->unsupported++;
      break;
    case WMTBlitCommandNop:
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      BLIT_ORDER();
      resident_buffer(r, body->src);
      resident_buffer(r, body->dst);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      BLIT_ORDER();
      resident_buffer(r, body->src);
      resident_texture(r, body->dst);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTextureWithBlitOption: {
      struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *body =
          (struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *)next;
      BLIT_ORDER();
      resident_buffer(r, body->src);
      resident_texture(r, body->dst);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                      options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      BLIT_ORDER();
      resident_texture(r, body->src);
      resident_buffer(r, body->dst);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBufferWithBlitOption: {
      struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *body =
          (struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *)next;
      BLIT_ORDER();
      resident_texture(r, body->src);
      resident_buffer(r, body->dst);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image
                           options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      BLIT_ORDER();
      resident_texture(r, body->src);
      resident_texture(r, body->dst);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->dst_slice
              destinationLevel:body->dst_level
             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      BLIT_ORDER();
      resident_texture(r, body->texture);
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      BLIT_ORDER();
      resident_buffer(r, body->buffer);
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResetCommandsInBuffer: {
      struct wmtcmd_blit_resetcommands *body = (struct wmtcmd_blit_resetcommands *)next;
      BLIT_ORDER();
      resident_object(r, body->indirect_command_buffer);
      [encoder resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                           withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTBlitCommandCopyTexture: {
      struct wmtcmd_blit_copy_texture *body = (struct wmtcmd_blit_copy_texture *)next;
      BLIT_ORDER();
      resident_texture(r, body->src);
      resident_texture(r, body->dst);
      [encoder copyFromTexture:(id<MTLTexture>)body->src toTexture:(id<MTLTexture>)body->dst];
      break;
    }
    }
    next = next->next.ptr;
  }
#undef BLIT_ORDER
  return STATUS_SUCCESS;
}

NTSTATUS
_MTL4CommandEncoder_barrier(void *obj) {
  struct unixcall_mtl4_barrier *params = obj;
  id<MTL4CommandEncoder> encoder = (id<MTL4CommandEncoder>)params->encoder;
  MTLStages after = (MTLStages)params->after_stages, before = (MTLStages)params->before_stages;
  MTL4VisibilityOptions visibility = (MTL4VisibilityOptions)params->visibility;
  switch ((enum WMTMtl4BarrierScope)params->scope) {
  case WMTMtl4BarrierAfterQueue:
    [encoder barrierAfterQueueStages:after beforeStages:before visibilityOptions:visibility];
    break;
  case WMTMtl4BarrierBeforeQueue:
    [encoder barrierAfterStages:after beforeQueueStages:before visibilityOptions:visibility];
    break;
  default:
    [encoder barrierAfterEncoderStages:after beforeEncoderStages:before visibilityOptions:visibility];
    break;
  }
  return STATUS_SUCCESS;
}
