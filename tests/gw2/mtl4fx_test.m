// gw2 (#56): native test of the Subsample MetalFX bridge (src/winemetal/unix/mtl4fx.c, slots 190-192).
// It calls the bridge's unix entry points directly, as winemetal.so does for the PE side: no Wine, no
// game, no window. Not part of the build; run it on the Mac:
//
//   T=$(mktemp -d)
//   for arch in x86_64 arm64; do
//     clang -x objective-c -std=gnu17 -fblocks -fno-objc-arc -O1 -arch $arch -mmacosx-version-min=26.0 \
//       -Isrc/winemetal -Isrc/winemetal/unix -Isrc/airconv \
//       src/winemetal/unix/mtl4fx.c tests/gw2/mtl4fx_test.m \
//       -framework Metal -framework MetalFX -framework Foundation -o $T/mtl4fx_test_$arch
//   done
//   $T/mtl4fx_test_x86_64                    # under Rosetta, as winemetal.so runs in the game
//   MTL_DEBUG_LAYER=1 $T/mtl4fx_test_arm64   # with Metal's validation layer
//
// 1. Formats: for the formats GW2's scene targets may have, a scaler for 1280x720 -> 3840x2160 (and
//    1920x1080 -> 3840x2160) on each path (Metal 3 MTLFXSpatialScaler, Metal 4 MTL4FXSpatialScaler),
//    and the texture usages MetalFX asks for (DXMT's front end checks them against the textures).
// 2. Upscale on each path, as DXMT encodes it: a synthetic 1280x720 RGBA8 image (a horizontal ramp
//    with a flat block) is uploaded, a "pass before" updates DXMT's fence, a blit encoder waits for
//    it and updates the scaler's fence, MetalFX waits for and updates that fence, a "pass after" waits
//    for it and reads the 3840x2160 output back. Metal 3: one MTLCommandBuffer. Metal 4: one
//    MTL4CommandBuffer on an MTL4CommandQueue with nothing resident except through the chunk set
//    that slot 192 fills (and the readback buffer). The output's flat block keeps its colour, the ramp
//    stays monotonic, and its corners match the input's.

#include <Metal/Metal.h>
#include <MetalFX/MetalFX.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "mtl4fx.h"

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

enum { IN_W = 1280, IN_H = 720, OUT_W = 3840, OUT_H = 2160 };

static id<MTLFXSpatialScalerBase>
new_scaler(id<MTLDevice> device, MTLPixelFormat in_fmt, MTLPixelFormat out_fmt, uint32_t in_w, uint32_t in_h,
           uint32_t mode, bool metal4, uint64_t *color_usage, uint64_t *output_usage) {
  struct WMTGW2SpatialScalerInfo info = {
      .color_format = (enum WMTPixelFormat)in_fmt,
      .output_format = (enum WMTPixelFormat)out_fmt,
      .input_width = in_w,
      .input_height = in_h,
      .output_width = OUT_W,
      .output_height = OUT_H,
      .color_processing_mode = mode,
      .metal4 = metal4,
  };
  struct unixcall_mtldevice_newgw2spatialscaler p = {.device = H(device)};
  p.info.ptr = &info;
  _MTLDevice_newGW2SpatialScaler(&p);
  if (color_usage)
    *color_usage = info.color_usage;
  if (output_usage)
    *output_usage = info.output_usage;
  return (id<MTLFXSpatialScalerBase>)p.ret;
}

static void
formats(id<MTLDevice> device, bool metal4_ok) {
  struct {
    MTLPixelFormat fmt;
    const char *name;
    uint32_t mode;
  } list[] = {
      {MTLPixelFormatRGBA8Unorm, "RGBA8Unorm", 0},       {MTLPixelFormatBGRA8Unorm, "BGRA8Unorm", 0},
      {MTLPixelFormatRGBA8Unorm_sRGB, "RGBA8Unorm_sRGB", 0}, {MTLPixelFormatBGRA8Unorm_sRGB, "BGRA8Unorm_sRGB", 0},
      {MTLPixelFormatRGB10A2Unorm, "RGB10A2Unorm", 0},   {MTLPixelFormatRG11B10Float, "RG11B10Float", 2},
      {MTLPixelFormatRGBA16Float, "RGBA16Float", 2},     {MTLPixelFormatRGBA32Float, "RGBA32Float", 2},
  };
  for (int metal4 = 0; metal4 < (metal4_ok ? 2 : 1); metal4++) {
    for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
      uint64_t cu = 0, ou = 0;
      id<MTLFXSpatialScalerBase> s =
          new_scaler(device, list[i].fmt, list[i].fmt, IN_W, IN_H, list[i].mode, metal4, &cu, &ou);
      id<MTLFXSpatialScalerBase> s2 =
          new_scaler(device, list[i].fmt, list[i].fmt, 1920, 1080, list[i].mode, metal4, NULL, NULL);
      printf("formats: %s %-16s mode %u: 720p %s, 1080p %s, colorUsage 0x%llx outputUsage 0x%llx\n",
             metal4 ? "Metal 4" : "Metal 3", list[i].name, list[i].mode, s ? "ok" : "refused",
             s2 ? "ok" : "refused", (unsigned long long)cu, (unsigned long long)ou);
      [s release];
      [s2 release];
    }
  }
}

static uint32_t
in_pixel(unsigned x, unsigned y) {
  // a flat block (x 400..799, y 200..499) of (40, 160, 220), else a horizontal ramp in R, G = y ramp
  if (x >= 400 && x < 800 && y >= 200 && y < 500)
    return 0xff000000u | (220u << 16) | (160u << 8) | 40u;
  uint32_t r = x * 255 / (IN_W - 1), g = y * 255 / (IN_H - 1);
  return 0xff000000u | (64u << 16) | (g << 8) | r;
}

static id<MTLTexture>
make_input(id<MTLDevice> device, MTLTextureUsage usage) {
  MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                              width:IN_W
                                                                             height:IN_H
                                                                          mipmapped:NO];
  d.usage = usage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  d.storageMode = MTLStorageModeShared;
  id<MTLTexture> t = [device newTextureWithDescriptor:d];
  uint32_t *px = malloc(IN_W * IN_H * 4);
  for (unsigned y = 0; y < IN_H; y++)
    for (unsigned x = 0; x < IN_W; x++)
      px[y * IN_W + x] = in_pixel(x, y);
  [t replaceRegion:MTLRegionMake2D(0, 0, IN_W, IN_H) mipmapLevel:0 withBytes:px bytesPerRow:IN_W * 4];
  free(px);
  return t;
}

static id<MTLTexture>
make_output(id<MTLDevice> device, MTLTextureUsage usage) {
  // as the front end creates it: a D3D11 DEFAULT texture with SRV | RTV | UAV binds (private storage)
  MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                              width:OUT_W
                                                                             height:OUT_H
                                                                          mipmapped:NO];
  d.usage = usage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
  d.storageMode = MTLStorageModePrivate;
  return [device newTextureWithDescriptor:d];
}

static void
check_output(const char *path, const uint32_t *out) {
  unsigned bad_block = 0, bad_ramp = 0;
  // the flat block, away from its edges (MetalFX sharpens edges): input x 420..779 -> 1260..2339
  for (unsigned y = 640; y < 1460; y += 7)
    for (unsigned x = 1300; x < 2300; x += 11) {
      uint32_t p = out[y * OUT_W + x];
      int r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff;
      if (abs(r - 40) > 3 || abs(g - 160) > 3 || abs(b - 220) > 3)
        bad_block++;
    }
  // the ramp along a row below the block (input y 600 -> 1800), R non-decreasing within 2 levels
  int prev = -1;
  for (unsigned x = 8; x < OUT_W - 8; x++) {
    int r = out[1800 * OUT_W + x] & 0xff;
    if (prev >= 0 && r + 2 < prev)
      bad_ramp++;
    prev = r;
  }
  uint32_t c0 = out[1800 * OUT_W + 8], c1 = out[1800 * OUT_W + OUT_W - 9];
  printf("upscale %s: block mismatches %u, ramp reversals %u, row 1800 R %u..%u, block centre 0x%08x\n", path,
         bad_block, bad_ramp, c0 & 0xff, c1 & 0xff, out[1050 * OUT_W + 1800]);
  CHECK(bad_block == 0, "%s: %u block pixels off", path, bad_block);
  CHECK(bad_ramp == 0, "%s: %u ramp reversals", path, bad_ramp);
  CHECK((c0 & 0xff) <= 4 && (c1 & 0xff) >= 250, "%s: ramp ends %u..%u", path, c0 & 0xff, c1 & 0xff);
}

static void
upscale_metal3(id<MTLDevice> device) {
  uint64_t cu = 0, ou = 0;
  id<MTLFXSpatialScaler> scaler = (id<MTLFXSpatialScaler>)new_scaler(
      device, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm, IN_W, IN_H, 0, false, &cu, &ou
  );
  CHECK(scaler != nil, "Metal 3 scaler");
  if (!scaler)
    return;
  id<MTLTexture> in = make_input(device, cu);
  id<MTLTexture> out = make_output(device, ou);
  id<MTLBuffer> rb = [device newBufferWithLength:OUT_W * OUT_H * 4 options:MTLResourceStorageModeShared];
  id<MTLFence> dxmt_fence = [device newFence], scaler_fence = [device newFence];
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  // as ArgumentEncodingContext::flushCommands' SpatialUpscale on Metal 3 (BeginScaler, scaler, EndScaler)
  id<MTLBlitCommandEncoder> before = [cb blitCommandEncoder];
  [before updateFence:dxmt_fence];
  [before endEncoding];
  id<MTLBlitCommandEncoder> begin = [cb blitCommandEncoder];
  [begin waitForFence:dxmt_fence];
  [begin updateFence:scaler_fence];
  [begin endEncoding];
  scaler.colorTexture = in;
  scaler.outputTexture = out;
  scaler.fence = scaler_fence;
  [scaler encodeToCommandBuffer:cb]; // what slot 59 (MTLCommandBuffer_encodeSpatialScale) does
  id<MTLBlitCommandEncoder> after = [cb blitCommandEncoder];
  [after waitForFence:scaler_fence];
  [after copyFromTexture:out
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(OUT_W, OUT_H, 1)
                        toBuffer:rb
               destinationOffset:0
          destinationBytesPerRow:OUT_W * 4
        destinationBytesPerImage:OUT_W * OUT_H * 4];
  [after endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  CHECK(cb.status == MTLCommandBufferStatusCompleted, "Metal 3 status %ld %s", (long)cb.status,
        cb.error ? [cb.error.description UTF8String] : "");
  if (cb.status == MTLCommandBufferStatusCompleted)
    check_output("Metal 3", rb.contents);
  [scaler release];
  [in release];
  [out release];
  [rb release];
  [dxmt_fence release];
  [scaler_fence release];
  [queue release];
}

static void
upscale_metal4(id<MTLDevice> device) API_AVAILABLE(macos(26.0)) {
  uint64_t cu = 0, ou = 0;
  id<MTL4FXSpatialScaler> scaler = (id<MTL4FXSpatialScaler>)new_scaler(
      device, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm, IN_W, IN_H, 0, true, &cu, &ou
  );
  CHECK(scaler != nil, "Metal 4 scaler");
  if (!scaler)
    return;
  id<MTLTexture> in = make_input(device, cu);
  id<MTLTexture> out = make_output(device, ou);
  id<MTLBuffer> rb = [device newBufferWithLength:OUT_W * OUT_H * 4 options:MTLResourceStorageModeShared];
  id<MTLFence> dxmt_fence = [device newFence], scaler_fence = [device newFence];

  id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
  id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
  id<MTL4CommandBuffer> cb = [device newCommandBuffer];
  MTLResidencySetDescriptor *rd = [[MTLResidencySetDescriptor alloc] init];
  NSError *err = nil;
  id<MTLResidencySet> chunk = [device newResidencySetWithDescriptor:rd error:&err];
  [rd release];
  [chunk addAllocation:rb]; // the readback is the test's own; the textures come from slot 192

  [cb beginCommandBufferWithAllocator:allocator];
  // as DXMT's Metal 4 SpatialUpscale: a blit (compute) encoder waits for DXMT's fences and updates the
  // scaler's, the scaler, then one that waits for the scaler's and updates DXMT's
  id<MTL4ComputeCommandEncoder> before = [cb computeCommandEncoder];
  [before updateFence:dxmt_fence afterEncoderStages:MTLStageBlit];
  [before endEncoding];
  id<MTL4ComputeCommandEncoder> begin = [cb computeCommandEncoder];
  [begin waitForFence:dxmt_fence beforeEncoderStages:MTLStageBlit];
  [begin updateFence:scaler_fence afterEncoderStages:MTLStageBlit];
  [begin endEncoding];

  struct unixcall_mtl4commandbuffer_spatial_scale p = {
      .cmdbuf = H(cb), .scaler = H(scaler), .color = H(in), .output = H(out), .fence = H(scaler_fence),
      .residency_set = H(chunk)
  };
  _MTL4CommandBuffer_encodeSpatialScale(&p);

  id<MTL4ComputeCommandEncoder> after = [cb computeCommandEncoder];
  [after waitForFence:scaler_fence beforeEncoderStages:MTLStageBlit];
  [after copyFromTexture:out
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(OUT_W, OUT_H, 1)
                        toBuffer:rb
               destinationOffset:0
          destinationBytesPerRow:OUT_W * 4
        destinationBytesPerImage:OUT_W * OUT_H * 4];
  [after endEncoding];

  [chunk commit]; // as Mtl4Queue::commit: the chunk set's changes, then useResidencySet
  [cb useResidencySet:chunk];
  [cb endCommandBuffer];

  __block NSError *feedback_error = nil;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  MTL4CommitOptions *opts = [[MTL4CommitOptions alloc] init];
  [opts addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
    feedback_error = [fb.error retain];
    dispatch_semaphore_signal(done);
  }];
  [queue commit:&cb count:1 options:opts];
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  [opts release];
  CHECK(feedback_error == nil, "Metal 4 commit error %s", [feedback_error.description UTF8String]);
  if (!feedback_error)
    check_output("Metal 4", rb.contents);
  printf("Metal 4: chunk set allocations %lu (input, output, readback)\n", (unsigned long)chunk.allocationCount);
  CHECK(chunk.allocationCount == 3, "chunk set has %lu allocations", (unsigned long)chunk.allocationCount);

  [feedback_error release];
  [scaler release];
  [in release];
  [out release];
  [rb release];
  [dxmt_fence release];
  [scaler_fence release];
  [chunk release];
  [cb release];
  [allocator release];
  [queue release];
}

int
main(void) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    printf("device: %s\n", [device.name UTF8String]);
    struct unixcall_generic_obj_uint64_ret s = {.handle = H(device)};
    _MTLDevice_supportsMetal4FX(&s);
    printf("supportsDevice (Metal 3 MetalFX): %d, supportsMetal4FX (slot 190): %llu\n",
           [MTLFXSpatialScalerDescriptor supportsDevice:device], (unsigned long long)s.ret);
    formats(device, s.ret != 0);
    upscale_metal3(device);
    if (s.ret) {
      if (@available(macOS 26.0, *))
        upscale_metal4(device);
    } else {
      CHECK(0, "no Metal 4 MetalFX on this Mac");
    }
    printf(failures ? "FAILED: %d\n" : "OK\n", failures);
    [device release];
  }
  return failures ? 1 : 0;
}
