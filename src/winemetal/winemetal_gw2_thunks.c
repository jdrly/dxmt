/*
 * gw2 (#291): my fork's PE thunks that have no upstream thunk to sit next to (thread priority,
 * Metal 4, MetalFX for GW2's Subsample pass). Not a translation unit of its own: winemetal_thunks.c
 * includes this file after its macros, so every build recipe that compiles winemetal_thunks.c gets
 * these thunks too. It isn't at winemetal_thunks.c's end because upstream appends every new thunk
 * there. A new thunk of that kind goes here; don't add this file to a source list.
 */

WINEMETAL_API int
WMTSetThreadQoSClassSelf(
    enum WMTQoSClass qos, int relative_priority, enum WMTQoSClass *old_qos, enum WMTQoSClass *new_qos
) {
  struct unixcall_wmt_set_thread_qos params;
  params.qos_class = qos;
  params.relative_priority = relative_priority;
  params.ret_old_qos_class = 0;
  params.ret_new_qos_class = 0;
  params.ret = -1;
  params.padding = 0;
  UNIX_CALL(WMTSetThreadQoSClassSelf, &params);
  if (old_qos)
    *old_qos = (enum WMTQoSClass)params.ret_old_qos_class;
  if (new_qos)
    *new_qos = (enum WMTQoSClass)params.ret_new_qos_class;
  return params.ret;
}

WINEMETAL_API int
WMTSetThreadBasePrioritySelf(int base_priority, int *old_base, int *new_base) {
  struct unixcall_wmt_set_thread_base_priority params;
  params.base_priority = base_priority;
  params.ret_old_base = -1;
  params.ret_new_base = -1;
  params.ret = -1;
  UNIX_CALL(WMTSetThreadBasePrioritySelf, &params);
  if (old_base)
    *old_base = params.ret_old_base;
  if (new_base)
    *new_base = params.ret_new_base;
  return params.ret;
}

// gw2 (#31)
WINEMETAL_API obj_handle_t
MTLCommandQueue_commandBufferWithUnretainedReferences(obj_handle_t queue) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = queue;
  params.ret = 0;
  UNIX_CALL(MTLCommandQueue_commandBufferWithUnretainedReferences, &params);
  return params.ret;
}

// gw2 (#32): Metal 4
WINEMETAL_API enum WMTMetal4Support
MTLDevice_metal4Support(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = WMTMetal4NoOS;
  UNIX_CALL(MTLDevice_metal4Support, &params);
  return (enum WMTMetal4Support)params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newMTL4CommandQueue(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_newMTL4CommandQueue, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newCommandAllocator(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_newCommandAllocator, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newMTL4CommandBuffer(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_newMTL4CommandBuffer, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newArgumentTable(obj_handle_t device, const struct WMTArgTableInfo *info, obj_handle_t *err_out) {
  struct unixcall_mtl4_new_argtable params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret = 0;
  UNIX_CALL(MTLDevice_newArgumentTable, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret;
}

WINEMETAL_API uint64_t
MTL4CommandAllocator_reset(obj_handle_t allocator) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = allocator;
  params.ret = 0;
  UNIX_CALL(MTL4CommandAllocator_reset, &params);
  return params.ret;
}

WINEMETAL_API void
MTL4CommandBuffer_beginCommandBuffer(obj_handle_t cmdbuf, obj_handle_t allocator) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = cmdbuf;
  params.arg = allocator;
  UNIX_CALL(MTL4CommandBuffer_beginCommandBuffer, &params);
}

WINEMETAL_API void
MTL4CommandBuffer_endCommandBuffer(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_noret params;
  params.handle = cmdbuf;
  UNIX_CALL(MTL4CommandBuffer_endCommandBuffer, &params);
}

WINEMETAL_API void
MTL4CommandBuffer_useResidencySet(obj_handle_t cmdbuf, obj_handle_t residency_set) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = cmdbuf;
  params.arg = residency_set;
  UNIX_CALL(MTL4CommandBuffer_useResidencySet, &params);
}

WINEMETAL_API void
MTL4CommandQueue_commit(
    obj_handle_t queue, const obj_handle_t *cmdbufs, uint64_t count, struct WMTMtl4CommitFeedback *feedback,
    obj_handle_t event, uint64_t event_value
) {
  struct unixcall_mtl4_queue_commit params;
  params.queue = queue;
  WMT_MEMPTR_SET(params.cmdbufs, cmdbufs);
  params.count = count;
  WMT_MEMPTR_SET(params.feedback, feedback);
  params.event = event;
  params.event_value = event_value;
  UNIX_CALL(MTL4CommandQueue_commit, &params);
}

WINEMETAL_API void
MTL4CommandQueue_waitForDrawable(obj_handle_t queue, obj_handle_t drawable) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = queue;
  params.arg = drawable;
  UNIX_CALL(MTL4CommandQueue_waitForDrawable, &params);
}

WINEMETAL_API void
MTL4CommandQueue_signalDrawable(obj_handle_t queue, obj_handle_t drawable) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = queue;
  params.arg = drawable;
  UNIX_CALL(MTL4CommandQueue_signalDrawable, &params);
}

WINEMETAL_API void
MTL4CommandQueue_signalEvent(obj_handle_t queue, obj_handle_t event, uint64_t value) {
  struct unixcall_generic_obj_obj_uint64_noret params;
  params.handle = queue;
  params.arg0 = event;
  params.arg1 = value;
  UNIX_CALL(MTL4CommandQueue_signalEvent, &params);
}

WINEMETAL_API void
MTL4CommandQueue_waitForEvent(obj_handle_t queue, obj_handle_t event, uint64_t value) {
  struct unixcall_generic_obj_obj_uint64_noret params;
  params.handle = queue;
  params.arg0 = event;
  params.arg1 = value;
  UNIX_CALL(MTL4CommandQueue_waitForEvent, &params);
}

WINEMETAL_API void
MTL4CommandQueue_addResidencySet(obj_handle_t queue, obj_handle_t residency_set) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = queue;
  params.arg = residency_set;
  UNIX_CALL(MTL4CommandQueue_addResidencySet, &params);
}

WINEMETAL_API void
MTL4CommandQueue_removeResidencySet(obj_handle_t queue, obj_handle_t residency_set) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = queue;
  params.arg = residency_set;
  UNIX_CALL(MTL4CommandQueue_removeResidencySet, &params);
}

WINEMETAL_API obj_handle_t
MetalLayer_residencySet(obj_handle_t layer) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = layer;
  params.ret = 0;
  UNIX_CALL(MetalLayer_residencySet, &params);
  return params.ret;
}

WINEMETAL_API void
MetalDrawable_present(obj_handle_t drawable, enum WMTDrawablePresentMode mode, double value) {
  struct unixcall_mtl4_drawable_present params;
  params.drawable = drawable;
  params.mode = mode;
  params.value = value;
  UNIX_CALL(MetalDrawable_present, &params);
}

WINEMETAL_API obj_handle_t
MTL4CommandBuffer_renderCommandEncoder(
    obj_handle_t cmdbuf, const struct WMTRenderPassInfo *info, struct WMTMtl4BindingState *state
) {
  struct unixcall_mtl4_new_encoder params;
  params.cmdbuf = cmdbuf;
  WMT_MEMPTR_SET(params.info, info);
  WMT_MEMPTR_SET(params.state, state);
  params.ret = 0;
  UNIX_CALL(MTL4CommandBuffer_renderCommandEncoder, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTL4CommandBuffer_computeCommandEncoder(obj_handle_t cmdbuf, struct WMTMtl4BindingState *state) {
  struct unixcall_mtl4_new_encoder params;
  params.cmdbuf = cmdbuf;
  WMT_MEMPTR_SET(params.info, NULL);
  WMT_MEMPTR_SET(params.state, state);
  params.ret = 0;
  UNIX_CALL(MTL4CommandBuffer_computeCommandEncoder, &params);
  return params.ret;
}

WINEMETAL_API void
MTL4RenderCommandEncoder_encodeCommands(
    obj_handle_t encoder, struct WMTMtl4BindingState *state, const struct wmtcmd_base *cmd_head
) {
  struct unixcall_mtl4_encode params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.state, state);
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(MTL4RenderCommandEncoder_encodeCommands, &params);
}

WINEMETAL_API void
MTL4ComputeCommandEncoder_encodeCommands(
    obj_handle_t encoder, struct WMTMtl4BindingState *state, const struct wmtcmd_base *cmd_head
) {
  struct unixcall_mtl4_encode params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.state, state);
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(MTL4ComputeCommandEncoder_encodeCommands, &params);
}

WINEMETAL_API void
MTL4ComputeCommandEncoder_encodeBlitCommands(
    obj_handle_t encoder, struct WMTMtl4BindingState *state, const struct wmtcmd_base *cmd_head
) {
  struct unixcall_mtl4_encode params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.state, state);
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(MTL4ComputeCommandEncoder_encodeBlitCommands, &params);
}

WINEMETAL_API void
MTL4CommandEncoder_barrier(
    obj_handle_t encoder, enum WMTMtl4BarrierScope scope, uint64_t after_stages, uint64_t before_stages,
    uint64_t visibility
) {
  struct unixcall_mtl4_barrier params;
  params.encoder = encoder;
  params.scope = scope;
  params.after_stages = after_stages;
  params.before_stages = before_stages;
  params.visibility = visibility;
  UNIX_CALL(MTL4CommandEncoder_barrier, &params);
}

/* gw2 (#56): Metal 4 MetalFX */
WINEMETAL_API bool
MTLDevice_supportsMetal4FX(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_supportsMetal4FX, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newGW2SpatialScaler(obj_handle_t device, struct WMTGW2SpatialScalerInfo *info) {
  struct unixcall_mtldevice_newgw2spatialscaler params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(MTLDevice_newGW2SpatialScaler, &params);
  return params.ret;
}

WINEMETAL_API void
MTL4CommandBuffer_encodeSpatialScale(
    obj_handle_t cmdbuf, obj_handle_t scaler, obj_handle_t color, obj_handle_t output, obj_handle_t fence,
    obj_handle_t residency_set
) {
  struct unixcall_mtl4commandbuffer_spatial_scale params;
  params.cmdbuf = cmdbuf;
  params.scaler = scaler;
  params.color = color;
  params.output = output;
  params.fence = fence;
  params.residency_set = residency_set;
  UNIX_CALL(MTL4CommandBuffer_encodeSpatialScale, &params);
}
