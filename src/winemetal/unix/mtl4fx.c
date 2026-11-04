/*
 * gw2 (#56): MetalFX spatial upscaling of GW2's Subsample pass (DXMT_SUBSAMPLE_METALFX), unix side.
 * Slots 190-192; the PE side is winemetal_thunks.c, the contract is in winemetal.h.
 *
 * Metal 3 encodes the scaler with the existing MTLCommandBuffer_encodeSpatialScale (slot 59). The
 * Metal 4 path needs an MTL4FXSpatialScaler: built with an MTL4Compiler, encoded into an
 * MTL4CommandBuffer. Metal 4 makes nothing resident by itself, so the input's and the output's
 * memory go into the chunk's residency set, as the replays in mtl4.c do for what packets name.
 */
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "mtl4fx.h"

#define STATUS_SUCCESS 0

NTSTATUS
_MTLDevice_supportsMetal4FX(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = 0;
  if (@available(macOS 26.0, *))
    params->ret = [MTLFXSpatialScalerDescriptor supportsMetal4FX:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

NTSTATUS
_MTLDevice_newGW2SpatialScaler(void *obj) {
  struct unixcall_mtldevice_newgw2spatialscaler *params = obj;
  struct WMTGW2SpatialScalerInfo *info = params->info.ptr;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  params->ret = NULL_OBJECT_HANDLE;

  MTLFXSpatialScalerDescriptor *desc = [[MTLFXSpatialScalerDescriptor alloc] init];
  desc.colorTextureFormat = (MTLPixelFormat)ORIGINAL_FORMAT(info->color_format);
  desc.outputTextureFormat = (MTLPixelFormat)ORIGINAL_FORMAT(info->output_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  desc.colorProcessingMode = (MTLFXSpatialScalerColorProcessingMode)info->color_processing_mode;

  id<MTLFXSpatialScalerBase> scaler = nil;
  if (info->metal4) {
    if (@available(macOS 26.0, *)) {
      MTL4CompilerDescriptor *compiler_desc = [[MTL4CompilerDescriptor alloc] init];
      NSError *err = nil;
      id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:compiler_desc error:&err];
      [compiler_desc release];
      if (compiler) {
        scaler = [desc newSpatialScalerWithDevice:device compiler:compiler];
        [compiler release];
      }
    }
  } else {
    scaler = [desc newSpatialScalerWithDevice:device];
  }
  [desc release];

  if (scaler) {
    info->color_usage = scaler.colorTextureUsage;
    info->output_usage = scaler.outputTextureUsage;
  }
  params->ret = (obj_handle_t)scaler;
  return STATUS_SUCCESS;
}

/* a texture's memory: a view's parent, a buffer-backed texture's buffer, or its heap (as mtl4.c) */
static void
resident_texture(id<MTLResidencySet> set, id<MTLTexture> texture) {
  while (texture.parentTexture)
    texture = texture.parentTexture;
  if (texture.buffer) {
    id<MTLBuffer> buffer = texture.buffer;
    [set addAllocation:buffer.heap ? (id<MTLAllocation>)buffer.heap : (id<MTLAllocation>)buffer];
    return;
  }
  [set addAllocation:texture.heap ? (id<MTLAllocation>)texture.heap : (id<MTLAllocation>)texture];
}

NTSTATUS
_MTL4CommandBuffer_encodeSpatialScale(void *obj) {
  struct unixcall_mtl4commandbuffer_spatial_scale *params = obj;
  if (@available(macOS 26.0, *)) {
    id<MTL4FXSpatialScaler> scaler = (id<MTL4FXSpatialScaler>)params->scaler;
    id<MTLTexture> color = (id<MTLTexture>)params->color;
    id<MTLTexture> output = (id<MTLTexture>)params->output;
    if (params->residency_set) {
      id<MTLResidencySet> set = (id<MTLResidencySet>)params->residency_set;
      resident_texture(set, color);
      resident_texture(set, output);
    }
    scaler.colorTexture = color;
    scaler.outputTexture = output;
    scaler.inputContentWidth = color.width;
    scaler.inputContentHeight = color.height;
    scaler.fence = (id<MTLFence>)params->fence;
    [scaler encodeToCommandBuffer:(id<MTL4CommandBuffer>)params->cmdbuf];
  }
  return STATUS_SUCCESS;
}
