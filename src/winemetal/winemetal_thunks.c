#ifdef _WIN32
#define WINEMETAL_API __declspec(dllexport)
#else
#define WINEMETAL_API
#endif

#include "winemetal_thunks.h"
#include <wineunixlib.h>
#include "assert.h"
#include "string.h"

/* gw2 (B3, #76): a thunk names its bridge call, UNIX_CALL(NSObject_retain, &obj); the name is pasted
   onto WMTCall_ (winemetal_call_numbers.h), so a literal number doesn't build. UNIX_CALL_STATUS
   returns the call's status, for the thunks that report a failure. */
#define UNIX_CALL_STATUS(call, params) WINE_UNIX_CALL(WMTCall_##call, params)
#ifdef NDEBUG
#define UNIX_CALL(call, params) WINE_UNIX_CALL(WMTCall_##call, params)
#else
#define UNIX_CALL(call, params)                                                                                        \
  {                                                                                                                    \
    NTSTATUS status = WINE_UNIX_CALL(WMTCall_##call, params);                                                          \
    assert(!status && "unix call failed");                                                                             \
  }
#endif

#define PtrToUInt64(v) ((uint64_t)(uintptr_t)(v))

WINEMETAL_API void
NSObject_retain(obj_handle_t obj) {
  UNIX_CALL(NSObject_retain, &obj);
}

WINEMETAL_API void
NSObject_release(obj_handle_t obj) {
  UNIX_CALL(NSObject_release, &obj);
}

WINEMETAL_API obj_handle_t
NSArray_object(obj_handle_t array, uint64_t index) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = array;
  params.arg = index;
  params.ret = 0;
  UNIX_CALL(NSArray_object, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
NSArray_count(obj_handle_t array) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = array;
  params.ret = 0;
  UNIX_CALL(NSArray_count, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
WMTCopyAllDevices() {
  struct unixcall_generic_obj_ret params;
  params.ret = 0;
  UNIX_CALL(MTLCopyAllDevices, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLDevice_recommendedMaxWorkingSetSize(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_recommendedMaxWorkingSetSize, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLDevice_currentAllocatedSize(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_currentAllocatedSize, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_name(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_name, &params);
  return params.ret;
}

WINEMETAL_API uint32_t
NSString_getCString(obj_handle_t str, char *buffer, uint64_t maxLength, enum WMTStringEncoding encoding) {
  struct unixcall_nsstring_getcstring params;
  params.str = str;
  params.buffer_ptr = PtrToUInt64(buffer);
  params.max_length = maxLength;
  params.encoding = encoding;
  UNIX_CALL(NSString_getCString, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newCommandQueue(obj_handle_t device, uint64_t maxCommandBufferCount) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = device;
  params.arg = maxCommandBufferCount;
  params.ret = 0;
  UNIX_CALL(MTLDevice_newCommandQueue, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
NSAutoreleasePool_alloc_init() {
  struct unixcall_generic_obj_ret params;
  params.ret = 0;
  UNIX_CALL(NSAutoreleasePool_alloc_init, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandQueue_commandBuffer(obj_handle_t queue) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = queue;
  params.ret = 0;
  UNIX_CALL(MTLCommandQueue_commandBuffer, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandBuffer_commit(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_noret params;
  params.handle = cmdbuf;
  UNIX_CALL(MTLCommandBuffer_commit, &params);
  return;
}

WINEMETAL_API void
MTLCommandBuffer_waitUntilCompleted(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_noret params;
  params.handle = cmdbuf;
  UNIX_CALL(MTLCommandBuffer_waitUntilCompleted, &params);
  return;
}

WINEMETAL_API enum WMTCommandBufferStatus
MTLCommandBuffer_status(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = cmdbuf;
  params.ret = 0;
  UNIX_CALL(MTLCommandBuffer_status, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSharedEvent(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_newSharedEvent, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLSharedEvent_signaledValue(obj_handle_t event) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = event;
  params.ret = 0;
  UNIX_CALL(MTLSharedEvent_signaledValue, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandBuffer_encodeSignalEvent(obj_handle_t cmdbuf, obj_handle_t event, uint64_t value) {
  struct unixcall_generic_obj_obj_uint64_noret params;
  params.handle = cmdbuf;
  params.arg0 = event;
  params.arg1 = value;
  UNIX_CALL(MTLCommandBuffer_encodeSignalEvent, &params);
  return;
}

WINEMETAL_API obj_handle_t
MTLDevice_newBuffer(obj_handle_t device, struct WMTBufferInfo *info) {
  struct unixcall_mtldevice_newbuffer params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_newBuffer, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSamplerState(obj_handle_t device, struct WMTSamplerInfo *info) {
  struct unixcall_mtldevice_newsamplerstate params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_newSamplerState, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newDepthStencilState(obj_handle_t device, const struct WMTDepthStencilInfo *info) {
  struct unixcall_mtldevice_newdepthstencilstate params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_newDepthStencilState, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newTexture(obj_handle_t device, struct WMTTextureInfo *info) {
  struct unixcall_mtldevice_newtexture params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_newTexture, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLBuffer_newTexture(obj_handle_t buffer, struct WMTTextureInfo *info, uint64_t offset, uint64_t bytes_per_row) {
  struct unixcall_mtlbuffer_newtexture params;
  params.buffer = buffer;
  WMT_MEMPTR_SET(params.info, info);
  params.offset = offset;
  params.bytes_per_row = bytes_per_row;
  UNIX_CALL(MTLBuffer_newTexture, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLTexture_newTextureView(
    obj_handle_t texture, enum WMTPixelFormat format, enum WMTTextureType texture_type, uint16_t level_start,
    uint16_t level_count, uint16_t slice_start, uint16_t slice_count, struct WMTTextureSwizzleChannels swizzle,
    uint64_t *out_gpu_resource_id
) {
  struct unixcall_mtltexture_newtextureview params;
  params.texture = texture;
  params.format = format;
  params.texture_type = texture_type;
  params.level_start = level_start;
  params.level_count = level_count;
  params.slice_start = slice_start;
  params.slice_count = slice_count;
  params.swizzle = swizzle;
  UNIX_CALL(MTLTexture_newTextureView, &params);
  *out_gpu_resource_id = params.gpu_resource_id;
  return params.ret;
}

WINEMETAL_API uint64_t
MTLDevice_minimumLinearTextureAlignmentForPixelFormat(obj_handle_t device, enum WMTPixelFormat format) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = device;
  params.arg = (uint64_t)format;
  params.ret = 0;
  UNIX_CALL(MTLDevice_minimumLinearTextureAlignmentForPixelFormat, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newLibrary(
    obj_handle_t device, obj_handle_t data, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newlibrary params;
  params.device = device;
  params.data = data;
  params.ret_error = 0;
  params.ret_library = 0;
  UNIX_CALL(MTLDevice_newLibrary, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_library;
}

WINEMETAL_API obj_handle_t
MTLLibrary_newFunction(obj_handle_t library, const char *name) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = library;
  params.arg = PtrToUInt64(name);
  params.ret = 0;
  UNIX_CALL(MTLLibrary_newFunction, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
NSString_lengthOfBytesUsingEncoding(obj_handle_t str, enum WMTStringEncoding encoding) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = str;
  params.arg = (uint64_t)(encoding);
  params.ret = 0;
  UNIX_CALL(NSString_lengthOfBytesUsingEncoding, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
NSObject_description(obj_handle_t nserror) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = nserror;
  UNIX_CALL(NSObject_description, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newComputePipelineState(
    obj_handle_t device, const struct WMTComputePipelineInfo *info, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newcomputepso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(MTLDevice_newComputePipelineState, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_blitCommandEncoder(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = cmdbuf;
  params.ret = 0;
  UNIX_CALL(MTLCommandBuffer_blitCommandEncoder, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_computeCommandEncoder(obj_handle_t cmdbuf, bool concurrent) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = cmdbuf;
  params.arg = concurrent;
  params.ret = 0;
  UNIX_CALL(MTLCommandBuffer_computeCommandEncoder, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_renderCommandEncoder(obj_handle_t cmdbuf, struct WMTRenderPassInfo *info) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = cmdbuf;
  params.arg = PtrToUInt64(info);
  params.ret = 0;
  UNIX_CALL(MTLCommandBuffer_renderCommandEncoder, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandEncoder_endEncoding(obj_handle_t encoder) {
  struct unixcall_generic_obj_noret params;
  params.handle = encoder;
  UNIX_CALL(MTLCommandEncoder_endEncoding, &params);
  return;
}

WINEMETAL_API obj_handle_t
MTLDevice_newRenderPipelineState(obj_handle_t device, const struct WMTRenderPipelineInfo *info, obj_handle_t *err_out) {
  struct unixcall_mtldevice_newrenderpso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(MTLDevice_newRenderPipelineState, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API obj_handle_t
MTLDevice_newMeshRenderPipelineState(
    obj_handle_t device, const struct WMTMeshRenderPipelineInfo *info, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newmeshrenderpso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(MTLDevice_newMeshRenderPipelineState, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API void
MTLBlitCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head) {
  struct unixcall_generic_obj_cmd_noret params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(MTLBlitCommandEncoder_encodeCommands, &params);
}

WINEMETAL_API void
MTLComputeCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head) {
  struct unixcall_generic_obj_cmd_noret params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(MTLComputeCommandEncoder_encodeCommands, &params);
}

WINEMETAL_API void
MTLRenderCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head) {
  struct unixcall_generic_obj_cmd_noret params;
  params.encoder = encoder;
  WMT_MEMPTR_SET(params.cmd_head, cmd_head);
  UNIX_CALL(MTLRenderCommandEncoder_encodeCommands, &params);
}

WINEMETAL_API enum WMTPixelFormat
MTLTexture_pixelFormat(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(MTLTexture_pixelFormat, &params);
  return (enum WMTPixelFormat)params.ret;
}

WINEMETAL_API uint64_t
MTLTexture_width(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(MTLTexture_width, &params);
  return params.ret;
}
WINEMETAL_API uint64_t
MTLTexture_height(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(MTLTexture_height, &params);
  return params.ret;
}
WINEMETAL_API uint64_t
MTLTexture_depth(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(MTLTexture_depth, &params);
  return params.ret;
}
WINEMETAL_API uint64_t
MTLTexture_arrayLength(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(MTLTexture_arrayLength, &params);
  return params.ret;
}
WINEMETAL_API uint64_t
MTLTexture_mipmapLevelCount(obj_handle_t texture) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = texture;
  params.ret = 0;
  UNIX_CALL(MTLTexture_mipmapLevelCount, &params);
  return params.ret;
}
WINEMETAL_API void
MTLTexture_replaceRegion(
    obj_handle_t texture, struct WMTOrigin origin, struct WMTSize size, uint64_t level, uint64_t slice,
    struct WMTMemoryPointer data, uint64_t bytes_per_row, uint64_t bytes_per_image
) {
  struct unixcall_mtltexture_replaceregion params;
  params.texture = texture;
  params.origin = origin;
  params.size = size;
  params.level = level;
  params.slice = slice;
  params.data = data;
  params.bytes_per_row = bytes_per_row;
  params.bytes_per_image = bytes_per_image;
  UNIX_CALL(MTLTexture_replaceRegion, &params);
}

WINEMETAL_API void
MTLBuffer_didModifyRange(obj_handle_t buffer, uint64_t start, uint64_t length) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = buffer;
  params.arg = start;
  params.ret = length;
  UNIX_CALL(MTLBuffer_didModifyRange, &params);
}

WINEMETAL_API void
MTLCommandBuffer_presentDrawable(obj_handle_t cmdbuf, obj_handle_t drawable) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = cmdbuf;
  params.arg = drawable;
  UNIX_CALL(MTLCommandBuffer_presentDrawable, &params);
};

WINEMETAL_API void
MTLCommandBuffer_presentDrawableAfterMinimumDuration(obj_handle_t cmdbuf, obj_handle_t drawable, double after) {
  struct unixcall_generic_obj_obj_double_noret params;
  params.handle = cmdbuf;
  params.arg0 = drawable;
  params.arg1 = after;
  UNIX_CALL(MTLCommandBuffer_presentDrawableAfterMinimumDuration, &params);
};

WINEMETAL_API void
MTLCommandBuffer_presentDrawableWithDeadline(obj_handle_t cmdbuf, obj_handle_t drawable, double interval) {
  struct unixcall_generic_obj_obj_double_noret params;
  params.handle = cmdbuf;
  params.arg0 = drawable;
  params.arg1 = interval;
  UNIX_CALL(MTLCommandBuffer_presentDrawableWithDeadline, &params);
};

// gw2 (#7)
WINEMETAL_API uint32_t
MTLDrawable_trackPresented(
    obj_handle_t drawable, uint64_t frame, struct WMTPresentedRecord *out, uint32_t capacity, uint32_t *lost
) {
  struct unixcall_mtldrawable_trackpresented params;
  params.drawable = drawable;
  params.frame = frame;
  params.capacity = capacity < WMT_PRESENTED_BATCH ? capacity : WMT_PRESENTED_BATCH;
  params.count = 0;
  params.lost = 0;
  UNIX_CALL(MTLDrawable_trackPresented, &params);
  uint32_t count = (uint32_t)params.count;
  for (uint32_t i = 0; i < count; i++)
    out[i] = params.records[i];
  if (lost)
    *lost = (uint32_t)params.lost;
  return count;
}

WINEMETAL_API bool
MTLDevice_supportsFamily(obj_handle_t device, enum WMTGPUFamily gpu_family) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = device;
  params.arg = gpu_family;
  params.ret = 0;
  UNIX_CALL(MTLDevice_supportsFamily, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_supportsBCTextureCompression(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_supportsBCTextureCompression, &params);
  return params.ret;
};

WINEMETAL_API bool
MTLDevice_supportsTextureSampleCount(obj_handle_t device, uint8_t sample_count) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = device;
  params.arg = sample_count;
  params.ret = 0;
  UNIX_CALL(MTLDevice_supportsTextureSampleCount, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_hasUnifiedMemory(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_hasUnifiedMemory, &params);
  return params.ret;
};

WINEMETAL_API obj_handle_t
MTLCaptureManager_sharedCaptureManager() {
  struct unixcall_generic_obj_ret params;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(MTLCaptureManager_sharedCaptureManager, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLCaptureManager_startCapture(obj_handle_t mgr, struct WMTCaptureInfo *info) {
  struct unixcall_mtlcapturemanager_startcapture params;
  params.capture_manager = mgr;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLCaptureManager_startCapture, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCaptureManager_stopCapture(obj_handle_t mgr) {
  struct unixcall_generic_obj_noret params;
  params.handle = mgr;
  UNIX_CALL(MTLCaptureManager_stopCapture, &params);
}

WINEMETAL_API obj_handle_t
MTLDevice_newTemporalScaler(obj_handle_t device, const struct WMTFXTemporalScalerInfo *info) {
  struct unixcall_mtldevice_newfxtemporalscaler params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_newTemporalScaler, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSpatialScaler(obj_handle_t device, const struct WMTFXSpatialScalerInfo *info) {
  struct unixcall_mtldevice_newfxspatialscaler params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_newSpatialScaler, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandBuffer_encodeTemporalScale(
    obj_handle_t cmdbuf, obj_handle_t scaler, obj_handle_t color, obj_handle_t output, obj_handle_t depth,
    obj_handle_t motion, obj_handle_t exposure, obj_handle_t fence, const struct WMTFXTemporalScalerProps *props
) {
  struct unixcall_mtlcommandbuffer_temporal_scale params;
  params.cmdbuf = cmdbuf;
  params.scaler = scaler;
  params.color = color;
  params.output = output;
  params.depth = depth;
  params.motion = motion;
  params.exposure = exposure;
  params.fence = fence;
  WMT_MEMPTR_SET(params.props, props);
  UNIX_CALL(MTLCommandBuffer_encodeTemporalScale, &params);
}

WINEMETAL_API void
MTLCommandBuffer_encodeSpatialScale(
    obj_handle_t cmdbuf, obj_handle_t scaler, obj_handle_t color, obj_handle_t output, obj_handle_t fence
) {
  struct unixcall_mtlcommandbuffer_spatial_scale params;
  params.cmdbuf = cmdbuf;
  params.scaler = scaler;
  params.color = color;
  params.output = output;
  params.fence = fence;
  UNIX_CALL(MTLCommandBuffer_encodeSpatialScale, &params);
}

WINEMETAL_API obj_handle_t
NSString_string(const char *data, enum WMTStringEncoding encoding) {
  struct unixcall_nsstring_string params;
  WMT_MEMPTR_SET(params.buffer_ptr, data);
  params.encoding = encoding;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(NSString_string, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
NSString_alloc_init(const char *data, enum WMTStringEncoding encoding) {
  struct unixcall_nsstring_string params;
  WMT_MEMPTR_SET(params.buffer_ptr, data);
  params.encoding = encoding;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(NSString_alloc_init, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
DeveloperHUDProperties_instance() {
  struct unixcall_generic_obj_ret params;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(DeveloperHUDProperties_instance, &params);
  return params.ret;
}

WINEMETAL_API bool
DeveloperHUDProperties_addLabel(obj_handle_t obj, obj_handle_t label, obj_handle_t after) {
  struct unixcall_generic_obj_obj_obj_uint64_ret params;
  params.handle = obj;
  params.arg0 = label;
  params.arg1 = after;
  params.ret = 0;
  UNIX_CALL(DeveloperHUDProperties_addLabel, &params);
  return params.ret;
}

WINEMETAL_API void
DeveloperHUDProperties_updateLabel(obj_handle_t obj, obj_handle_t label, obj_handle_t value) {
  struct unixcall_generic_obj_obj_obj_noret params;
  params.handle = obj;
  params.arg0 = label;
  params.arg1 = value;
  UNIX_CALL(DeveloperHUDProperties_updateLabel, &params);
}

WINEMETAL_API void
DeveloperHUDProperties_remove(obj_handle_t obj, obj_handle_t label) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = obj;
  params.arg = label;
  UNIX_CALL(DeveloperHUDProperties_remove, &params);
}

WINEMETAL_API obj_handle_t
MetalDrawable_texture(obj_handle_t drawable) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = drawable;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(MetalDrawable_texture, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MetalLayer_nextDrawable(obj_handle_t layer) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = layer;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(MetalLayer_nextDrawable, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLDevice_supportsFXSpatialScaler(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_supportsFXSpatialScaler, &params);
  return params.ret;
};

WINEMETAL_API bool
MTLDevice_supportsFXTemporalScaler(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_supportsFXTemporalScaler, &params);
  return params.ret;
};

WINEMETAL_API void
MetalLayer_setProps(obj_handle_t layer, const struct WMTLayerProps *props) {
  struct unixcall_generic_obj_constptr_noret params;
  params.handle = layer;
  WMT_MEMPTR_SET(params.arg, props);
  UNIX_CALL(MetalLayer_setProps, &params);
}

WINEMETAL_API void
MetalLayer_getProps(obj_handle_t layer, struct WMTLayerProps *props) {
  struct unixcall_generic_obj_ptr_noret params;
  params.handle = layer;
  WMT_MEMPTR_SET(params.arg, props);
  UNIX_CALL(MetalLayer_getProps, &params);
}

WINEMETAL_API obj_handle_t
CreateMetalViewFromHWND(intptr_t hwnd, obj_handle_t device, obj_handle_t *layer) {
  struct unixcall_create_metal_view_from_hwnd params;
  params.hwnd = (uint64_t)(uintptr_t)hwnd;
  params.device = device;
  params.ret_layer = NULL_OBJECT_HANDLE;
  params.ret_view = NULL_OBJECT_HANDLE;
  UNIX_CALL(CreateMetalViewFromHWND, &params);
  *layer = params.ret_layer;
  return params.ret_view;
}

WINEMETAL_API void
ReleaseMetalView(obj_handle_t view) {
  struct unixcall_generic_obj_noret params;
  params.handle = view;
  UNIX_CALL(ReleaseMetalView, &params);
}

WINEMETAL_API uint64_t
MetalView_visibility(obj_handle_t view) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = view;
  params.ret = 0;
  UNIX_CALL(MetalView_visibility, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandEncoder_setLabel(obj_handle_t encoder, obj_handle_t label) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = encoder;
  params.arg = label;
  UNIX_CALL(MTLCommandEncoder_setLabel, &params);
}

WINEMETAL_API void
MTLDevice_setShouldMaximizeConcurrentCompilation(obj_handle_t device, bool value) {
  struct unixcall_generic_obj_uint64_noret params;
  params.handle = device;
  params.arg = value;
  UNIX_CALL(MTLDevice_setShouldMaximizeConcurrentCompilation, &params);
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_error(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = cmdbuf;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(MTLCommandBuffer_error, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_logs(obj_handle_t cmdbuf) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = cmdbuf;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(MTLCommandBuffer_logs, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLLogContainer_enumerate(obj_handle_t logs, uint64_t start, uint64_t buffer_size, obj_handle_t *buffer) {
  struct unixcall_enumerate params;
  params.enumerable = logs;
  params.start = start;
  params.buffer_size = buffer_size;
  WMT_MEMPTR_SET(params.buffer, buffer);
  params.ret_read = 0;
  UNIX_CALL(MTLLogContainer_enumerate, &params);
  return params.ret_read;
}

WINEMETAL_API bool
CGColorSpace_checkColorSpaceSupported(enum WMTColorSpace colorspace) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = colorspace;
  params.ret = 0;
  UNIX_CALL(CGColorSpace_checkColorSpaceSupported, &params);
  return params.ret;
}

WINEMETAL_API bool
MetalLayer_setColorSpace(obj_handle_t layer, enum WMTColorSpace colorspace) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = layer;
  params.arg = colorspace;
  params.ret = 0;
  UNIX_CALL(MetalLayer_setColorSpace, &params);
  return params.ret;
}

WINEMETAL_API uint32_t
WMTGetPrimaryDisplayId() {
  struct unixcall_generic_obj_ret params;
  params.ret = 0;
  UNIX_CALL(WMTGetPrimaryDisplayId, &params);
  return params.ret;
}

WINEMETAL_API uint32_t
WMTGetSecondaryDisplayId() {
  struct unixcall_generic_obj_ret params;
  params.ret = 0;
  UNIX_CALL(WMTGetSecondaryDisplayId, &params);
  return params.ret;
}

WINEMETAL_API void
WMTGetDisplayDescription(uint32_t display_id, struct WMTDisplayDescription *desc) {
  struct unixcall_generic_obj_ptr_noret params;
  params.handle = display_id;
  WMT_MEMPTR_SET(params.arg, desc);
  UNIX_CALL(WMTGetDisplayDescription, &params);
}

WINEMETAL_API void
MetalLayer_getEDRValue(obj_handle_t layer, struct WMTEDRValue *value) {
  struct unixcall_generic_obj_constptr_noret params;
  params.handle = layer;
  WMT_MEMPTR_SET(params.arg, value);
  UNIX_CALL(MetalLayer_getEDRValue, &params);
}

WINEMETAL_API obj_handle_t
MTLLibrary_newFunctionWithConstants(
    obj_handle_t library, const char *name, const struct WMTFunctionConstant *constants, uint32_t num_constants,
    obj_handle_t *err_out
) {
  struct unixcall_mtllibrary_newfunction_with_constants params;
  params.library = library;
  WMT_MEMPTR_SET(params.name, name);
  WMT_MEMPTR_SET(params.constants, constants);
  params.num_constants = num_constants;
  params.ret = 0;
  params.ret_error = 0;
  UNIX_CALL(MTLLibrary_newFunctionWithConstants, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret;
}

WINEMETAL_API bool
WMTQueryDisplaySetting(uint32_t display_id, enum WMTColorSpace *colorspace, struct WMTHDRMetadata *metadata) {
  struct unixcall_query_display_setting params;
  params.display_id = display_id;
  WMT_MEMPTR_SET(params.hdr_metadata, metadata);
  UNIX_CALL(WMTQueryDisplaySetting, &params);
  *colorspace = params.colorspace;
  return params.ret;
}

WINEMETAL_API void
WMTUpdateDisplaySetting(uint32_t display_id, enum WMTColorSpace colorspace, const struct WMTHDRMetadata *metadata) {
  struct unixcall_update_display_setting params;
  params.display_id = display_id;
  params.colorspace = colorspace;
  WMT_MEMPTR_SET(params.hdr_metadata, metadata);
  UNIX_CALL(WMTUpdateDisplaySetting, &params);
}

WINEMETAL_API void
WMTQueryDisplaySettingForLayer(
    obj_handle_t layer, uint64_t *version, enum WMTColorSpace *colorspace, struct WMTHDRMetadata *metadata,
    struct WMTEDRValue *edr_value
) {
  struct unixcall_query_display_setting_for_layer params;
  params.layer = layer;
  WMT_MEMPTR_SET(params.hdr_metadata, metadata);
  UNIX_CALL(WMTQueryDisplaySettingForLayer, &params);
  *version = params.version;
  *colorspace = params.colorspace;
  *edr_value = params.edr_value;
}

WINEMETAL_API void
MTLCommandBuffer_encodeWaitForEvent(obj_handle_t cmdbuf, obj_handle_t event, uint64_t value) {
  struct unixcall_generic_obj_obj_uint64_noret params;
  params.handle = cmdbuf;
  params.arg0 = event;
  params.arg1 = value;
  UNIX_CALL(MTLCommandBuffer_encodeWaitForEvent, &params);
  return;
}

WINEMETAL_API void
MTLSharedEvent_signalValue(obj_handle_t event, uint64_t value) {
  struct unixcall_generic_obj_uint64_noret params;
  params.handle = event;
  params.arg = value;
  UNIX_CALL(MTLSharedEvent_signalValue, &params);
}

WINEMETAL_API void
MTLSharedEvent_setWin32EventAtValue(
    obj_handle_t event, obj_handle_t shared_event_listener, void *nt_event_handle, uint64_t at_value
) {
  struct unixcall_mtlsharedevent_setevent params;
  params.shared_event = event;
  params.shared_event_listener = shared_event_listener;
  params.event_handle = (obj_handle_t)PtrToUInt64(nt_event_handle);
  params.value = at_value;
  UNIX_CALL(MTLSharedEvent_setWin32EventAtValue, &params);
}

WINEMETAL_API obj_handle_t
MTLDevice_newFence(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_newFence, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newEvent(obj_handle_t device) {
  struct unixcall_generic_obj_obj_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_newEvent, &params);
  return params.ret;
}

WINEMETAL_API void
MTLBuffer_updateContents(obj_handle_t buffer, uint64_t offset, struct WMTConstMemoryPointer data, uint64_t length) {
  struct unixcall_mtlbuffer_updatecontents params;
  params.buffer = buffer;
  params.offset = offset;
  params.data = data;
  params.length = length;
  UNIX_CALL(MTLBuffer_updateContents, &params);
}

WINEMETAL_API obj_handle_t
SharedEventListener_create() {
  struct unixcall_generic_obj_ret params;
  UNIX_CALL(SharedEventListener_create, &params);
  return params.ret;
}

WINEMETAL_API void
SharedEventListener_start(obj_handle_t event_queue) {
  struct unixcall_generic_obj_noret params;
  params.handle = event_queue;
  UNIX_CALL(SharedEventListener_start, &params);
}

WINEMETAL_API void
SharedEventListener_destroy(obj_handle_t event_queue) {
  struct unixcall_generic_obj_noret params;
  params.handle = event_queue;
  UNIX_CALL(SharedEventListener_destroy, &params);
}

WINEMETAL_API void
WMTGetOSVersion(uint64_t *major, uint64_t *minor, uint64_t *patch) {
  struct unixcall_get_os_version params;
  UNIX_CALL(WMTGetOSVersion, &params);
  if (major)
    *major = params.ret_major;
  if (minor)
    *minor = params.ret_minor;
  if (patch)
    *patch = params.ret_patch;
}

WINEMETAL_API obj_handle_t
MTLDevice_newBinaryArchive(obj_handle_t device, const char *url, obj_handle_t *err_out) {
  struct unixcall_mtldevice_newbinaryarchive params;
  params.device = device;
  WMT_MEMPTR_SET(params.url, url);
  params.ret_archive = 0;
  params.ret_error = 0;
  UNIX_CALL(MTLDevice_newBinaryArchive, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_archive;
}

WINEMETAL_API void
MTLBinaryArchive_serialize(obj_handle_t archive, const char *url, obj_handle_t *err_out) {
  struct unixcall_mtlbinaryarchive_serialize params;
  params.archive = archive;
  WMT_MEMPTR_SET(params.url, url);
  UNIX_CALL(MTLBinaryArchive_serialize, &params);
  if (err_out)
    *err_out = params.ret_error;
}

WINEMETAL_API obj_handle_t
DispatchData_alloc_init(uint64_t native_ptr, uint64_t length) {
  struct unixcall_generic_obj_uint64_obj_ret params;
  params.handle = native_ptr;
  params.arg = length;
  UNIX_CALL(DispatchData_alloc_init, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
CacheReader_alloc_init(const char *path, uint64_t version) {
  struct unixcall_cache_alloc_init params;
  WMT_MEMPTR_SET(params.path, path);
  params.version = version;
  UNIX_CALL(CacheReader_alloc_init, &params);
  return params.ret_cache;
}

WINEMETAL_API obj_handle_t
CacheReader_get(obj_handle_t reader, const void *key, uint64_t length) {
  struct unixcall_cache_get params;
  params.cache = reader;
  WMT_MEMPTR_SET(params.key, key);
  params.key_length = length;
  UNIX_CALL(CacheReader_get, &params);
  return params.ret_data;
}

WINEMETAL_API obj_handle_t
CacheWriter_alloc_init(const char *path, uint64_t version) {
  struct unixcall_cache_alloc_init params;
  WMT_MEMPTR_SET(params.path, path);
  params.version = version;
  UNIX_CALL(CacheWriter_alloc_init, &params);
  return params.ret_cache;
}

WINEMETAL_API bool
CacheWriter_set(
    obj_handle_t writer, const void *key, uint64_t key_length, obj_handle_t value
) {
  struct unixcall_cache_set params;
  params.cache = writer;
  WMT_MEMPTR_SET(params.key, key);
  params.key_length = key_length;
  params.value_data = value;
  params.query_size = 0;
  params.ret_success = 0;
  params.ret_db_bytes = 0;
  UNIX_CALL(CacheWriter_set, &params);
  return params.ret_success != 0;
}

WINEMETAL_API uint32_t
CacheEnumerate(const char *path, uint64_t version, uint32_t prefix, uint64_t after_rowid, void *buffer,
               uint64_t capacity, struct WMTCacheEnumerateResult *result) {
  struct unixcall_cache_enumerate params;
  WMT_MEMPTR_SET(params.path, path);
  params.version = version;
  params.after_rowid = after_rowid;
  WMT_MEMPTR_SET(params.buffer, buffer);
  params.capacity = capacity;
  params.prefix = prefix;
  params.ret_count = 0;
  params.ret_used = 0;
  params.ret_last_rowid = after_rowid;
  params.ret_more = 0;
  params.ret_status = 1;
  params.ret_error = 0;
  params.ret_busy = 0;
  params.ret_need = 0;
  params.ret_need_rowid = 0;
  UNIX_CALL(CacheEnumerate, &params);
  result->used = params.ret_used;
  result->last_rowid = params.ret_last_rowid;
  result->need = params.ret_need;
  result->need_rowid = params.ret_need_rowid;
  result->count = params.ret_count;
  result->status = params.ret_status;
  result->error = params.ret_error;
  result->busy = params.ret_busy;
  result->more = params.ret_more != 0;
  return params.ret_count;
}

WINEMETAL_API uint64_t
CacheWriter_byteSize(obj_handle_t writer) {
  struct unixcall_cache_set params;
  params.cache = writer;
  WMT_MEMPTR_SET(params.key, NULL);
  params.key_length = 0;
  params.value_data = 0;
  params.query_size = 1;
  params.ret_success = 0;
  params.ret_db_bytes = 0;
  UNIX_CALL(CacheWriter_set, &params);
  return params.ret_db_bytes;
}

WINEMETAL_API bool
WMTSetMetalShaderCachePath(const char *path) {
  struct unixcall_setmetalcachepath params;
  WMT_MEMPTR_SET(params.path, path);
  params.ret_success = 0;
  UNIX_CALL(WMTSetMetalShaderCachePath, &params);
  return params.ret_success;
}

/* gw2 (#46) */
WINEMETAL_API uint32_t
WMTGetMetalShaderCachePath(char *buf, uint32_t size) {
  struct unixcall_getmetalcachepath params;
  WMT_MEMPTR_SET(params.buf, buf);
  params.size = size;
  params.ret_length = 0;
  if (buf && size)
    buf[0] = '\0';
  UNIX_CALL(WMTGetMetalShaderCachePath, &params);
  return (uint32_t)params.ret_length;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSharedTexture(obj_handle_t device, struct WMTTextureInfo *info) {
  struct unixcall_mtldevice_newtexture params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_newSharedTexture, &params);
  return params.ret;
}

WINEMETAL_API bool
WMTBootstrapRegister(const char *name, mach_port_t mach_port) {
  struct unixcall_bootstrap params;
  strncpy(params.name, name, sizeof(params.name) - 1);
  params.name[sizeof(params.name) - 1] = '\0';
  params.mach_port = mach_port;
  NTSTATUS ret = UNIX_CALL_STATUS(WMTBootstrapRegister, &params);
  return !ret;
}

WINEMETAL_API bool
WMTBootstrapLookUp(const char *name, mach_port_t *mach_port) {
  struct unixcall_bootstrap params;
  strncpy(params.name, name, sizeof(params.name) - 1);
  params.name[sizeof(params.name) - 1] = '\0';
  params.mach_port = 0;
  NTSTATUS ret = UNIX_CALL_STATUS(WMTBootstrapLookUp, &params);
  *mach_port = params.mach_port;
  return !ret;
}

WINEMETAL_API mach_port_t
MTLSharedEvent_createMachPort(obj_handle_t event) {
  struct unixcall_mtlsharedevent_createmachport params;
  params.event = event;
  params.ret_mach_port = 0;
  UNIX_CALL(MTLSharedEvent_createMachPort, &params);
  return params.ret_mach_port;
}

WINEMETAL_API obj_handle_t
MTLDevice_newSharedEventWithMachPort(obj_handle_t device, mach_port_t mach_port) {
  struct unixcall_mtldevice_newsharedeventwithmachport params;
  params.device = device;
  params.mach_port = mach_port;
  params.ret_event = 0;
  UNIX_CALL(MTLDevice_newSharedEventWithMachPort, &params);
  return params.ret_event;
}

WINEMETAL_API uint64_t
MTLDevice_registryID(obj_handle_t device) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = device;
  params.ret = 0;
  UNIX_CALL(MTLDevice_registryID, &params);
  return params.ret;
}

WINEMETAL_API bool
MTLSharedEvent_waitUntilSignaledValue(obj_handle_t event, uint64_t value, uint64_t timeout) {
  struct unixcall_mtlsharedevent_waituntilsignaledvalue params;
  params.event = event;
  params.value = value;
  params.timeout_ms = timeout;
  params.ret_timeout = 0;
  UNIX_CALL(MTLSharedEvent_waitUntilSignaledValue, &params);
  return params.ret_timeout;
}

WINEMETAL_API obj_handle_t
MTLCounterSampleBuffer_newTimestampBuffer(obj_handle_t device, uint32_t sample_count, bool shared) {
  struct unixcall_mtlcountersamplebuffer_newtimestampbuffer params;
  params.device = device;
  params.sample_count = sample_count;
  params.shared = shared;
  params.ret = 0;
  UNIX_CALL(MTLCounterSampleBuffer_newTimestampBuffer, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCounterSampleBuffer_resolveCounterRange(
    obj_handle_t sample_buffer, uint32_t start, uint32_t len, void *data_out, uint64_t data_length
) {
  struct unixcall_mtlcountersamplebuffer_resolvecounterrange params;
  params.sample_buffer = sample_buffer;
  params.start = start;
  params.len = len;
  params.data_length = data_length;
  WMT_MEMPTR_SET(params.data_out, data_out);
  UNIX_CALL(MTLCounterSampleBuffer_resolveCounterRange, &params);
}

WINEMETAL_API obj_handle_t
MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(
    obj_handle_t cmdbuf, struct WMTSampleBufferAttachmentInfo *sample_buffer_attachments,
    uint64_t num_sample_buffer_attachments) {
  struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers params;
  params.cmdbuf = cmdbuf;
  WMT_MEMPTR_SET(params.attachments, sample_buffer_attachments);
  params.num_attachments = num_sample_buffer_attachments;
  params.ret = 0;
  UNIX_CALL(MTLCommandBuffer_blitCommandEncoderWithSampleBuffers, &params);
  return params.ret;
}

WINEMETAL_API uint64_t
MTLCommandBuffer_property(obj_handle_t cmdbuf, enum WMTCommandBufferProperty prop) {
  struct unixcall_generic_obj_uint64_uint64_ret params;
  params.handle = cmdbuf;
  params.arg = prop;
  UNIX_CALL(MTLCommandBuffer_property, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newTileRenderPipelineState(
    obj_handle_t device, const struct WMTTileRenderPipelineInfo *info, obj_handle_t *err_out
) {
  struct unixcall_mtldevice_newrenderpso params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = 0;
  params.ret_pso = 0;
  UNIX_CALL(MTLDevice_newTileRenderPipelineState, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_pso;
}

WINEMETAL_API obj_handle_t
MTLDevice_newResidencySet(obj_handle_t device, uint64_t init_capacity, obj_handle_t *err_out) {
  struct unixcall_mtldevice_newresidencyset params;
  params.device = device;
  params.init_capacity = init_capacity;
  params.ret_error = 0;
  params.ret_set = 0;
  UNIX_CALL(MTLDevice_newResidencySet, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_set;
}

WINEMETAL_API void
MTLResidencySet_addAllocations(obj_handle_t residency_set, const obj_handle_t *allocations, uint64_t count) {
  struct unixcall_mtlresidencyset_addallocations params;
  params.set = residency_set;
  WMT_MEMPTR_SET(params.allocations, allocations);
  params.count = count;
  UNIX_CALL(MTLResidencySet_addAllocations, &params);
}

WINEMETAL_API void
MTLResidencySet_removeAllocations(obj_handle_t residency_set, const obj_handle_t *allocations, uint64_t count) {
  struct unixcall_mtlresidencyset_addallocations params;
  params.set = residency_set;
  WMT_MEMPTR_SET(params.allocations, allocations);
  params.count = count;
  UNIX_CALL(MTLResidencySet_removeAllocations, &params);
}

WINEMETAL_API void
MTLResidencySet_removeAllAllocations(obj_handle_t residency_set) {
  struct unixcall_generic_obj_noret params;
  params.handle = residency_set;
  UNIX_CALL(MTLResidencySet_removeAllAllocations, &params);
}

WINEMETAL_API void
MTLResidencySet_commit(obj_handle_t residency_set) {
  struct unixcall_generic_obj_noret params;
  params.handle = residency_set;
  UNIX_CALL(MTLResidencySet_commit, &params);
}

WINEMETAL_API uint64_t
MTLResidencySet_allocationCount(obj_handle_t residency_set) {
  struct unixcall_generic_obj_uint64_ret params;
  params.handle = residency_set;
  params.ret = 0;
  UNIX_CALL(MTLResidencySet_allocationCount, &params);
  return params.ret;
}

WINEMETAL_API void
MTLCommandQueue_addResidencySet(obj_handle_t queue, obj_handle_t residency_set) {
  struct unixcall_generic_obj_obj_noret params;
  params.handle = queue;
  params.arg = residency_set;
  UNIX_CALL(MTLCommandQueue_addResidencySet, &params);
}

WINEMETAL_API obj_handle_t
MTLDevice_newHeap(obj_handle_t device, const struct WMTHeapInfo *info) {
  struct unixcall_mtldevice_newheap params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_newHeap, &params);
  return params.ret;
}

WINEMETAL_API struct WMTSizeAndAlign
MTLDevice_heapBufferSizeAndAlign(obj_handle_t device, uint64_t length, enum WMTResourceOptions options) {
  struct unixcall_mtldevice_heapbuffersizeandalign params;
  params.device = device;
  params.length = length;
  params.options = options;
  UNIX_CALL(MTLDevice_heapBufferSizeAndAlign, &params);
  struct WMTSizeAndAlign ret;
  ret.size = params.ret_size;
  ret.align = params.ret_align;
  return ret;
}

WINEMETAL_API struct WMTSizeAndAlign
MTLDevice_heapTextureSizeAndAlign(obj_handle_t device, struct WMTTextureInfo *info) {
  struct unixcall_mtldevice_heaptexturesizeandalign params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  UNIX_CALL(MTLDevice_heapTextureSizeAndAlign, &params);
  struct WMTSizeAndAlign ret;
  ret.size = params.ret_size;
  ret.align = params.ret_align;
  return ret;
}

WINEMETAL_API obj_handle_t
MTLHeap_newBuffer(obj_handle_t heap, struct WMTBufferInfo *info, uint64_t offset) {
  struct unixcall_mtlheap_newbuffer params;
  params.heap = heap;
  WMT_MEMPTR_SET(params.info, info);
  params.offset = offset;
  UNIX_CALL(MTLHeap_newBuffer, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLHeap_newTexture(obj_handle_t heap, struct WMTTextureInfo *info, uint64_t offset) {
  struct unixcall_mtlheap_newtexture params;
  params.heap = heap;
  WMT_MEMPTR_SET(params.info, info);
  params.offset = offset;
  UNIX_CALL(MTLHeap_newTexture, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newIndirectCommandBuffer(
    obj_handle_t device, struct WMTIndirectCommandBufferInfo *info, uint64_t max_count, enum WMTResourceOptions options
) {
  struct unixcall_mtldevice_newicb params;
  params.device = device;
  WMT_MEMPTR_SET(params.info, info);
  params.max_count = max_count;
  params.options = options;
  UNIX_CALL(MTLDevice_newIndirectCommandBuffer, &params);
  return params.ret;
}

WINEMETAL_API obj_handle_t
MTLDevice_newLibraryWithSource(obj_handle_t device, const char *source, uint64_t length, obj_handle_t *err_out) {
  struct unixcall_mtldevice_newlibrarywithsource params;
  params.device = device;
  WMT_MEMPTR_SET(params.source, source);
  params.length = length;
  params.ret_error = 0;
  params.ret_library = 0;
  UNIX_CALL(MTLDevice_newLibraryWithSource, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret_library;
}

WINEMETAL_API void
MTLTexture_getBytes(
    obj_handle_t texture, struct WMTOrigin origin, struct WMTSize size, uint64_t level, uint64_t slice,
    struct WMTMemoryPointer data, uint64_t bytes_per_row, uint64_t bytes_per_image
) {
  // use compatible struct
  struct unixcall_mtltexture_replaceregion params;
  params.texture = texture;
  params.origin = origin;
  params.size = size;
  params.level = level;
  params.slice = slice;
  params.data = data;
  params.bytes_per_row = bytes_per_row;
  params.bytes_per_image = bytes_per_image;
  UNIX_CALL(MTLTexture_getBytes, &params);
}

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

// gw2 (#5): PSO archives
WINEMETAL_API bool
WMTPSOArchiveInit(obj_handle_t device, const struct WMTPSOArchiveConfig *config, char *report, uint32_t report_size) {
  struct unixcall_wmt_pso_archive_init params;
  params.device = device;
  WMT_MEMPTR_SET(params.config, config);
  WMT_MEMPTR_SET(params.report, report);
  params.report_size = report_size;
  params.ret = 0;
  UNIX_CALL(WMTPSOArchiveInit, &params);
  return params.ret;
}

WINEMETAL_API int
WMTPSOArchiveFlush(char *report, uint32_t report_size) {
  struct unixcall_wmt_pso_archive_flush params;
  WMT_MEMPTR_SET(params.report, report);
  params.report_size = report_size;
  params.ret = -1;
  UNIX_CALL(WMTPSOArchiveFlush, &params);
  return (int)params.ret;
}

WINEMETAL_API void
WMTPSOArchiveQuery(struct WMTPSOArchiveStats *stats) {
  struct unixcall_wmt_pso_archive_query params;
  WMT_MEMPTR_SET(params.stats, stats);
  UNIX_CALL(WMTPSOArchiveQuery, &params);
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
