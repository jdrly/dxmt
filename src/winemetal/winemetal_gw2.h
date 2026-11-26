/*
 * gw2 (#291): my fork's bridge declarations that have no upstream declaration to sit next to: thread
 * priority (#9, #151), Metal 4 (#32) and MetalFX for GW2's Subsample pass (#56).
 * winemetal.h includes this file in its middle. It isn't at winemetal.h's end because upstream
 * appends every new bridge call there. A new declaration of that kind goes here.
 */
#ifndef _WINEMETAL_GW2_H
#define _WINEMETAL_GW2_H

#include "winemetal.h"

/* gw2 (#9): macOS QoS classes, the values of qos_class_t (<sys/qos.h>). */
enum WMTQoSClass {
  WMTQoSClassUnspecified = 0x00,
  WMTQoSClassBackground = 0x09,
  WMTQoSClassUtility = 0x11,
  WMTQoSClassDefault = 0x15,
  WMTQoSClassUserInitiated = 0x19,
  WMTQoSClassUserInteractive = 0x21,
};

/* gw2 (#9): pthread_set_qos_class_self_np(qos, relative_priority) on the calling thread (a unix
   call runs on the thread that makes it). Returns 0 or the errno; *old_qos is qos_class_self()
   before the call, *new_qos after it. On failure the thread is unchanged. */
WINEMETAL_API int WMTSetThreadQoSClassSelf(
    enum WMTQoSClass qos, int relative_priority, enum WMTQoSClass *old_qos, enum WMTQoSClass *new_qos
);

/* gw2 (#151): sets the calling thread's Mach base priority to base_priority (20 is the utility
   band, which macOS keeps on the P cores) with a THREAD_PRECEDENCE_POLICY call, as the heat ntdll
   does for HEAT_QOS=1=utility. pthread QoS is no use on a Wine thread: macOS refuses it once the
   wineserver has set Mach policies. Returns 0 or the kern_return_t; -1 if the task's priority
   couldn't be read (nothing set). *old_base and *new_base are the thread's base priority before
   and after (-1 if unreadable). */
WINEMETAL_API int WMTSetThreadBasePrioritySelf(int base_priority, int *old_base, int *new_base);

/*
 * gw2 (#32): Metal 4 (DXMT_METAL4); its call numbers are in winemetal_calls.h. The unix side is
 * src/winemetal/unix/mtl4.c. DXMT's encoders keep building today's wmtcmd_* packets: the
 * MTL4*_encodeCommands calls replay them on Metal 4 encoders, with buffer and texture bindings going
 * into argument tables (WMTMtl4BindingState). Every Metal 4 object here is created +1 (new*) or
 * autoreleased like its Metal 3 counterpart (the encoders); the caller keeps what the GPU uses
 * alive until the work retires: a Metal 4 command buffer retains nothing. Everything the GPU uses,
 * pipeline states included (they are MTLAllocations), must be in a residency set on the queue or
 * the command buffer: Metal 4 has no useResource.
 */

/* why a device can't take the Metal 4 path; 0 = it can */
enum WMTMetal4Support : uint32_t {
  WMTMetal4Supported = 0,
  WMTMetal4NoOS = 1,     /* macOS before 26 */
  WMTMetal4NoFamily = 2, /* not MTLGPUFamilyMetal4 */
  WMTMetal4NoQueue = 3,  /* newMTL4CommandQueue returned nil */
};

WINEMETAL_API enum WMTMetal4Support MTLDevice_metal4Support(obj_handle_t device);

/* the MTLStages bits (MTLCommandEncoder.h); the render ones equal WMTRenderStages */
enum WMTStages : uint64_t {
  WMTStageVertex = 1ull << 0,
  WMTStageFragment = 1ull << 1,
  WMTStageTile = 1ull << 2,
  WMTStageObject = 1ull << 3,
  WMTStageMesh = 1ull << 4,
  WMTStageResourceState = 1ull << 26,
  WMTStageDispatch = 1ull << 27,
  WMTStageBlit = 1ull << 28,
  WMTStageAccelerationStructure = 1ull << 29,
  WMTStageMachineLearning = 1ull << 30,
  WMTStageAll = 0x7fffffffffffffffull,
};

/* MTL4VisibilityOptions */
enum WMTMtl4Visibility : uint64_t {
  WMTMtl4VisibilityNone = 0,
  WMTMtl4VisibilityDevice = 1ull << 0,
  WMTMtl4VisibilityResourceAlias = 1ull << 1,
};

/* which MTL4CommandEncoder barrier: within the encoder, after earlier queue work, or before later */
enum WMTMtl4BarrierScope : uint32_t {
  WMTMtl4BarrierEncoder = 0,    /* barrierAfterEncoderStages:beforeEncoderStages: */
  WMTMtl4BarrierAfterQueue = 1, /* barrierAfterQueueStages:beforeStages: */
  WMTMtl4BarrierBeforeQueue = 2 /* barrierAfterStages:beforeQueueStages: */
};

struct WMTArgTableInfo {
  uint32_t max_buffers;  /* MTL4ArgumentTableDescriptor.maxBufferBindCount (at most 31) */
  uint32_t max_textures; /* maxTextureBindCount (at most 128) */
  uint32_t max_samplers; /* maxSamplerStateBindCount (at most 16) */
  uint8_t initialize_bindings;
  uint8_t support_attribute_strides;
  uint8_t padding[2];
};

/* the argument table of each stage of a Metal 4 encoder; a compute encoder uses the first */
enum WMTMtl4TableStage : uint32_t {
  WMTMtl4TableVertex = 0,
  WMTMtl4TableFragment = 1,
  WMTMtl4TableObject = 2,
  WMTMtl4TableMesh = 3,
  WMTMtl4TableCompute = 0,
};
#define WMT_MTL4_TABLE_STAGES 4
#define WMT_MTL4_MAX_BUFFERS 31
/* gw2 (#297): the texture slots of an argument table (newTable's max_textures); set_texture
   refuses an index at or past it */
#define WMT_MTL4_MAX_TEXTURES 32

/*
 * An encoder's binding state, in the caller's memory (the encode thread's), read and updated by the
 * MTL4*_encodeCommands replay. Today's packets bind a buffer once (Set*Buffer) and then move its
 * offset (Set*BufferOffset), so the replay keeps each index's buffer address here across calls. The
 * caller sets tables, bytes_* and zeroes the rest before an encoder's first call.
 */
struct WMTMtl4BindingState {
  obj_handle_t tables[WMT_MTL4_TABLE_STAGES]; /* by WMTMtl4TableStage; 0 = that stage isn't bound */
  /* gpuAddress of the buffer the last Set*Buffer bound at each index (0 = none) */
  uint64_t buffer_base[WMT_MTL4_TABLE_STAGES][WMT_MTL4_MAX_BUFFERS];
  /* Metal 4 has no setBytes: Set*Bytes copy into this shared buffer, from bytes_offset up to
     bytes_end (256-byte aligned), and bind its address. The caller keeps it alive until the work
     retires. 0 = no space: such packets count in bytes_overflow and are skipped. */
  obj_handle_t bytes_buffer;
  uint64_t bytes_offset;
  uint64_t bytes_end;
  /* gw2 (#32 part 3): a residency set (the chunk's) that gets every allocation the packets name
     directly: pipelines, bound buffers and textures, index and indirect buffers, blit operands, the
     bytes buffer, UseResource's resources (a heap-placed resource: its heap), and at encoder
     creation the attachments and the visibility buffer (unless flags has
     WMT_MTL4_NO_ATTACHMENT_RESIDENCY: the present pass's drawable). Metal 4 makes nothing resident
     by itself. The caller commits the set before the commit that needs it and empties it once the
     work retired. 0: no set; UseResource then counts in use_resource_dropped. gw2 (#153): what
     another set already holds stays out: a packet's WMT_CMD_RESIDENT and the bytes buffer with
     WMT_MTL4_BYTES_RESIDENT (the device's set, on the queue), the attachments with
     WMT_MTL4_NO_ATTACHMENT_RESIDENCY. */
  obj_handle_t residency_set;
  /* gw2 (#32 part 3): a Set*Bytes that didn't fit between bytes_offset and bytes_end (with a bytes
     buffer set) stops the replay at that packet: resume is set to it and bytes_needed to what it
     needs. The caller gives the state a new bytes buffer, clears resume and calls again from it, so
     no update is skipped. Cleared (0) when the replay reached the end. */
  struct WMTMemoryPointer resume;
  uint64_t bytes_needed;
  /* the compute pipeline's threadgroup size (its SetPSO packet's), for the dispatches after it, also
     across calls (a resumed replay); zeroed when a compute encoder is created */
  struct WMTSize threadgroup_size;
  uint32_t flags;
  /* counters, for the caller's statistics; it resets them */
  uint32_t use_resource_dropped; /* UseResource packets without a residency set (a miss) */
  uint32_t unsupported;          /* packets the replay can't encode on Metal 4 (skipped) */
  uint32_t bytes_overflow;       /* Set*Bytes with no bytes buffer at all (skipped) */
  uint32_t blit_barriers;        /* barriers between dependent copies (see encodeBlitCommands) */
  uint32_t residency_adds;       /* allocations added to residency_set (repeats within a call skipped) */
};
STATIC_ASSERT(sizeof(struct WMTMtl4BindingState) == 1120); /* the same on i386 and x86_64 */
/* the attachments (colors, resolves, depth, stencil) stay out of residency_set: another set holds
   them. A drawable: the layer's set, which the present pass's command buffer uses (gw2 #184; it was
   on the queue before). gw2 #153: other attachments: the device's set, on the queue. The visibility
   buffer still goes in. */
#define WMT_MTL4_NO_ATTACHMENT_RESIDENCY 0x1u
/* gw2 (#153): bytes_buffer stays out of residency_set: the device's set (on the queue) holds it */
#define WMT_MTL4_BYTES_RESIDENT 0x2u

/* written by the commit's feedback handler (on a Metal thread) into the caller's memory: first the
   times and the error, then done = 1, then the event (if any) is set to event_value. So a waiter on
   the event sees the rest. error is retained (the caller releases it) or 0. */
struct WMTMtl4CommitFeedback {
  double gpu_start; /* seconds, CACurrentMediaTime's clock */
  double gpu_end;
  obj_handle_t error;
  uint64_t done;
};
STATIC_ASSERT(sizeof(struct WMTMtl4CommitFeedback) == 32);
STATIC_ASSERT(sizeof(struct WMTArgTableInfo) == 16);

/* gw2 (#32): every value below is +1 (new*) unless it says otherwise */
WINEMETAL_API obj_handle_t MTLDevice_newMTL4CommandQueue(obj_handle_t device);
WINEMETAL_API obj_handle_t MTLDevice_newCommandAllocator(obj_handle_t device);
WINEMETAL_API obj_handle_t MTLDevice_newMTL4CommandBuffer(obj_handle_t device);
WINEMETAL_API obj_handle_t
MTLDevice_newArgumentTable(obj_handle_t device, const struct WMTArgTableInfo *info, obj_handle_t *err_out);

/* resets the allocator (its command buffers' work must have completed); returns its allocatedSize
   before the reset */
WINEMETAL_API uint64_t MTL4CommandAllocator_reset(obj_handle_t allocator);

WINEMETAL_API void MTL4CommandBuffer_beginCommandBuffer(obj_handle_t cmdbuf, obj_handle_t allocator);
WINEMETAL_API void MTL4CommandBuffer_endCommandBuffer(obj_handle_t cmdbuf);
WINEMETAL_API void MTL4CommandBuffer_useResidencySet(obj_handle_t cmdbuf, obj_handle_t residency_set);

/* commit:count:options: with one MTL4CommitOptions per call (a reused one runs its handlers for its
   first commit only, d9mt's measurement) whose handler fills *feedback and then sets event to
   event_value (event: an MTLSharedEvent; 0 = none). feedback 0: no handler. */
WINEMETAL_API void MTL4CommandQueue_commit(
    obj_handle_t queue, const obj_handle_t *cmdbufs, uint64_t count, struct WMTMtl4CommitFeedback *feedback,
    obj_handle_t event, uint64_t event_value
);
WINEMETAL_API void MTL4CommandQueue_waitForDrawable(obj_handle_t queue, obj_handle_t drawable);
WINEMETAL_API void MTL4CommandQueue_signalDrawable(obj_handle_t queue, obj_handle_t drawable);
WINEMETAL_API void MTL4CommandQueue_signalEvent(obj_handle_t queue, obj_handle_t event, uint64_t value);
WINEMETAL_API void MTL4CommandQueue_waitForEvent(obj_handle_t queue, obj_handle_t event, uint64_t value);
WINEMETAL_API void MTL4CommandQueue_addResidencySet(obj_handle_t queue, obj_handle_t residency_set);
WINEMETAL_API void MTL4CommandQueue_removeResidencySet(obj_handle_t queue, obj_handle_t residency_set);

/* CAMetalLayer.residencySet (not retained; 0 before macOS 26): the drawables' residency. A
   driver-owned set must not hold drawable textures (d9mt: it pins them and starves nextDrawable). */
WINEMETAL_API obj_handle_t MetalLayer_residencySet(obj_handle_t layer);

/* present without a command buffer (Metal 4: after signalDrawable). mode 0: present, 1:
   presentAfterMinimumDuration:value, 2: presentAtTime:(now + value) */
enum WMTDrawablePresentMode : uint32_t {
  WMTDrawablePresent = 0,
  WMTDrawablePresentAfterMinimumDuration = 1,
  WMTDrawablePresentWithDeadline = 2,
};
WINEMETAL_API void MetalDrawable_present(obj_handle_t drawable, enum WMTDrawablePresentMode mode, double value);

/* autoreleased, like Metal 3's encoders. Binds state's tables (if state isn't null); a render
   encoder also puts the pass's attachments into state's residency set */
WINEMETAL_API obj_handle_t MTL4CommandBuffer_renderCommandEncoder(
    obj_handle_t cmdbuf, const struct WMTRenderPassInfo *info, struct WMTMtl4BindingState *state
);
WINEMETAL_API obj_handle_t
MTL4CommandBuffer_computeCommandEncoder(obj_handle_t cmdbuf, struct WMTMtl4BindingState *state);

/* today's wmtcmd_render_* packets on an MTL4RenderCommandEncoder: Set*Buffer(Offset) and
   Set*Texture go into state's tables, Set*Bytes into its bytes buffer, fences to the MTL4 fence
   calls (the render stage bits are MTLStages'), MemoryBarrier to an encoder barrier, buffers of
   draws to their GPU addresses; UseResource is dropped (counted) */
WINEMETAL_API void MTL4RenderCommandEncoder_encodeCommands(
    obj_handle_t encoder, struct WMTMtl4BindingState *state, const struct wmtcmd_base *cmd_head
);
/* today's wmtcmd_compute_* packets on an MTL4ComputeCommandEncoder (dispatches of one encoder run
   concurrently, as DXMT's concurrent Metal 3 encoders; MemoryBarrier orders them) */
WINEMETAL_API void MTL4ComputeCommandEncoder_encodeCommands(
    obj_handle_t encoder, struct WMTMtl4BindingState *state, const struct wmtcmd_base *cmd_head
);
/* today's wmtcmd_blit_* packets on an MTL4ComputeCommandEncoder (Metal 4 has no blit encoder). Its
   copies may overlap, so the replay puts a blit-to-blit barrier between consecutive ones, keeping
   the Metal 3 blit encoder's order. ResolveCounters (Metal 3 counter sample buffers) is unsupported */
WINEMETAL_API void MTL4ComputeCommandEncoder_encodeBlitCommands(
    obj_handle_t encoder, struct WMTMtl4BindingState *state, const struct wmtcmd_base *cmd_head
);
WINEMETAL_API void MTL4CommandEncoder_barrier(
    obj_handle_t encoder, enum WMTMtl4BarrierScope scope, uint64_t after_stages, uint64_t before_stages,
    uint64_t visibility
);

/* gw2 (#56): MetalFX spatial upscaling of GW2's Subsample pass (DXMT_SUBSAMPLE_METALFX), slots
   190-192 */
struct WMTGW2SpatialScalerInfo {
  enum WMTPixelFormat color_format;
  enum WMTPixelFormat output_format;
  uint32_t input_width;
  uint32_t input_height;
  uint32_t output_width;
  uint32_t output_height;
  uint32_t color_processing_mode; /* MTLFXSpatialScalerColorProcessingMode: 0 perceptual, 1 linear, 2 HDR */
  uint32_t metal4;                /* 1: an MTL4FXSpatialScaler (built with an MTL4Compiler) */
  uint64_t color_usage;           /* out: the scaler's colorTextureUsage (WMTTextureUsage bits) */
  uint64_t output_usage;          /* out: the scaler's outputTextureUsage */
};
STATIC_ASSERT(sizeof(struct WMTGW2SpatialScalerInfo) == 48);

/* MTLFXSpatialScalerDescriptor supportsMetal4FX: (false before macOS 26) */
WINEMETAL_API bool MTLDevice_supportsMetal4FX(obj_handle_t device);
/* +1, or 0 if MetalFX refuses the descriptor (a format it doesn't take); fills the usages */
WINEMETAL_API obj_handle_t MTLDevice_newGW2SpatialScaler(obj_handle_t device, struct WMTGW2SpatialScalerInfo *info);
/* an MTL4FXSpatialScaler's work into an open MTL4CommandBuffer, outside any encoder. It waits for
   and updates fence (MetalFX's own synchronisation of untracked resources). color's and output's
   memory go into residency_set (the chunk's; committed with the command buffer) */
WINEMETAL_API void MTL4CommandBuffer_encodeSpatialScale(
    obj_handle_t cmdbuf, obj_handle_t scaler, obj_handle_t color, obj_handle_t output, obj_handle_t fence,
    obj_handle_t residency_set
);

#endif
