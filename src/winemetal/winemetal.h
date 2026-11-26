#ifndef _WINEMETAL_H
#define _WINEMETAL_H

#ifdef __cplusplus
#include <cstdint>
#include <cassert>
#define STATIC_ASSERT(x) static_assert(x)
#else
#include <stdint.h>
#include <stdbool.h>
#define STATIC_ASSERT(x)
#endif

#ifdef _WIN32
#define WINEMETAL_IMPORT __declspec(dllimport)
#else
#define WINEMETAL_IMPORT
#endif

#ifndef WINEMETAL_API
#ifdef __cplusplus
#define WINEMETAL_API extern "C" WINEMETAL_IMPORT
#else
#define WINEMETAL_API WINEMETAL_IMPORT
#endif
#endif

typedef uint64_t obj_handle_t;

#define NULL_OBJECT_HANDLE 0

#ifndef _MACH_PORT_T
typedef uint32_t mach_port_t;
#endif

WINEMETAL_API void NSObject_retain(obj_handle_t obj);

WINEMETAL_API void NSObject_release(obj_handle_t obj);

WINEMETAL_API obj_handle_t NSArray_object(obj_handle_t array, uint64_t index);

WINEMETAL_API uint64_t NSArray_count(obj_handle_t array);

WINEMETAL_API obj_handle_t WMTCopyAllDevices();

enum WMTStringEncoding : uint64_t {
  WMTASCIIStringEncoding = 1,
  WMTNEXTSTEPStringEncoding = 2,
  WMTJapaneseEUCStringEncoding = 3,
  WMTUTF8StringEncoding = 4,
  WMTWMTISOLatin1StringEncoding = 5,
  SymbolStringEncoding = 6,
  WMTNonLossyASCIIStringEncoding = 7,
  WMTShiftJISStringEncoding = 8,
  WMTISOLatin2StringEncoding = 9,
  WMTUnicodeStringEncoding = 10,
  WMTWindowsCP1251StringEncoding = 11,
  WMTWindowsCP1252StringEncoding = 12,
  WMTWindowsCP1253StringEncoding = 13,
  WMTWindowsCP1254StringEncoding = 14,
  WMTWindowsCP1250StringEncoding = 15,
  WMTISO2022JPStringEncoding = 21,
  WMTMacOSRomanStringEncoding = 30,

  WMTUTF16StringEncoding = WMTUnicodeStringEncoding,

  WMTUTF16BigEndianStringEncoding = 0x90000100,
  WMTUTF16LittleEndianStringEncoding = 0x94000100,

  WMTUTF32StringEncoding = 0x8c000100,
  WMTUTF32BigEndianStringEncoding = 0x98000100,
  WMTUTF32LittleEndianStringEncoding = 0x9c000100
};

WINEMETAL_API uint64_t MTLDevice_recommendedMaxWorkingSetSize(obj_handle_t device);

WINEMETAL_API uint64_t MTLDevice_currentAllocatedSize(obj_handle_t device);

WINEMETAL_API obj_handle_t MTLDevice_name(obj_handle_t device);

WINEMETAL_API uint32_t
NSString_getCString(obj_handle_t str, char *buffer, uint64_t maxLength, enum WMTStringEncoding encoding);

WINEMETAL_API obj_handle_t MTLDevice_newCommandQueue(obj_handle_t device, uint64_t maxCommandBufferCount);

WINEMETAL_API obj_handle_t NSAutoreleasePool_alloc_init();

WINEMETAL_API obj_handle_t MTLCommandQueue_commandBuffer(obj_handle_t queue);

/* gw2 (#31): [queue commandBufferWithUnretainedReferences] (bridge call 154): a command buffer that
   doesn't retain the objects its commands use; the caller keeps them alive until it completes.
   Autoreleased, as MTLCommandQueue_commandBuffer. */
WINEMETAL_API obj_handle_t MTLCommandQueue_commandBufferWithUnretainedReferences(obj_handle_t queue);

WINEMETAL_API void MTLCommandBuffer_commit(obj_handle_t cmdbuf);

WINEMETAL_API void MTLCommandBuffer_waitUntilCompleted(obj_handle_t cmdbuf);

enum WMTCommandBufferStatus : uint64_t {
  WMTCommandBufferStatusNotEnqueued = 0,
  WMTCommandBufferStatusEnqueued = 1,
  WMTCommandBufferStatusCommitted = 2,
  WMTCommandBufferStatusScheduled = 3,
  WMTCommandBufferStatusCompleted = 4,
  WMTCommandBufferStatusError = 5,
};

WINEMETAL_API enum WMTCommandBufferStatus MTLCommandBuffer_status(obj_handle_t cmdbuf);

WINEMETAL_API obj_handle_t MTLDevice_newSharedEvent(obj_handle_t device);

WINEMETAL_API uint64_t MTLSharedEvent_signaledValue(obj_handle_t event);

WINEMETAL_API void MTLCommandBuffer_encodeSignalEvent(obj_handle_t cmdbuf, obj_handle_t event, uint64_t value);

enum WMTResourceOptions : uint64_t {
  WMTResourceCPUCacheModeDefaultCache = 0,
  WMTResourceCPUCacheModeWriteCombined = 1,
  WMTResourceStorageModeShared = 0,
  WMTResourceStorageModeManaged = 16,
  WMTResourceStorageModePrivate = 32,
  WMTResourceStorageModeMemoryless = 48,
  WMTResourceHazardTrackingModeDefault = 0,
  WMTResourceHazardTrackingModeUntracked = 256,
  WMTResourceHazardTrackingModeTracked = 512,
  WMTResourceOptionCPUCacheModeDefault = 0,
  WMTResourceOptionCPUCacheModeWriteCombined = 1,
};

struct WMTMemoryPointer {
  void *ptr;
#if defined(__i386__)
  uint32_t high_part;
#endif

#ifdef __cplusplus
  void
  set(void *p) {
#if defined(__i386__)
    high_part = 0;
#endif
    ptr = p;
  }

  void *
  get() {
#if defined(__i386__)
    assert(!high_part && "inaccessible 64-bit pointer");
#endif
    return ptr;
  }

  void *
  get_accessible_or_null() {
#if defined(__i386__)
    if (high_part)
      return nullptr;
#endif
    return ptr;
  }
#endif
};

struct WMTConstMemoryPointer {
  const void *ptr;
#if defined(__i386__)
  uint32_t high_part;
#endif

#ifdef __cplusplus
  void
  set(const void *p) {
#if defined(__i386__)
    high_part = 0;
#endif
    ptr = p;
  }

  const void *
  get() {
#if defined(__i386__)
    assert(!high_part && "inaccessible 64-bit pointer");
#endif
    return ptr;
  }
#endif
};

#if defined(__i386__)
#define WMT_MEMPTR_SET(obj, value)                                                                                     \
  obj.high_part = 0;                                                                                                   \
  obj.ptr = value
#else
#define WMT_MEMPTR_SET(obj, value) obj.ptr = value
#endif

STATIC_ASSERT(sizeof(WMTMemoryPointer) == 8);
STATIC_ASSERT(sizeof(WMTConstMemoryPointer) == 8);

struct WMTBufferInfo {
  uint64_t length;                 // in
  enum WMTResourceOptions options; // in
  struct WMTMemoryPointer memory;  // inout
  uint64_t gpu_address;            // out
};

STATIC_ASSERT(sizeof(WMTBufferInfo) == 32);

WINEMETAL_API obj_handle_t MTLDevice_newBuffer(obj_handle_t device, struct WMTBufferInfo *info);

enum WMTSamplerBorderColor : uint8_t {
  WMTSamplerBorderColorTransparentBlack = 0,
  WMTSamplerBorderColorOpaqueBlack = 1,
  WMTSamplerBorderColorOpaqueWhite = 2,
};

enum WMTSamplerAddressMode : uint8_t {
  WMTSamplerAddressModeClampToEdge = 0,
  WMTSamplerAddressModeMirrorClampToEdge = 1,
  WMTSamplerAddressModeRepeat = 2,
  WMTSamplerAddressModeMirrorRepeat = 3,
  WMTSamplerAddressModeClampToZero = 4,
  WMTSamplerAddressModeClampToBorderColor = 5,
};

enum WMTSamplerMipFilter : uint8_t {
  WMTSamplerMipFilterNotMipmapped = 0,
  WMTSamplerMipFilterNearest = 1,
  WMTSamplerMipFilterLinear = 2,
};

enum WMTSamplerMinMagFilter : uint8_t {
  WMTSamplerMinMagFilterNearest = 0,
  WMTSamplerMinMagFilterLinear = 1,
};

enum WMTCompareFunction : uint8_t {
  WMTCompareFunctionNever = 0,
  WMTCompareFunctionLess = 1,
  WMTCompareFunctionEqual = 2,
  WMTCompareFunctionLessEqual = 3,
  WMTCompareFunctionGreater = 4,
  WMTCompareFunctionNotEqual = 5,
  WMTCompareFunctionGreaterEqual = 6,
  WMTCompareFunctionAlways = 7,
};

struct WMTSamplerInfo {
  enum WMTSamplerMinMagFilter min_filter;
  enum WMTSamplerMinMagFilter mag_filter;
  enum WMTSamplerMipFilter mip_filter;
  enum WMTSamplerAddressMode r_address_mode;
  enum WMTSamplerAddressMode s_address_mode;
  enum WMTSamplerAddressMode t_address_mode;
  enum WMTSamplerBorderColor border_color;
  enum WMTCompareFunction compare_function;
  float lod_min_clamp;
  float lod_max_clamp;
  uint32_t max_anisotroy;
  bool normalized_coords;
  bool lod_average;
  bool support_argument_buffers;
  uint64_t gpu_resource_id; // out
};

STATIC_ASSERT(sizeof(WMTSamplerInfo) == 32);

WINEMETAL_API obj_handle_t MTLDevice_newSamplerState(obj_handle_t device, struct WMTSamplerInfo *info);

enum WMTStencilOperation : uint8_t {
  WMTStencilOperationKeep = 0,
  WMTStencilOperationZero = 1,
  WMTStencilOperationReplace = 2,
  WMTStencilOperationIncrementClamp = 3,
  WMTStencilOperationDecrementClamp = 4,
  WMTStencilOperationInvert = 5,
  WMTStencilOperationIncrementWrap = 6,
  WMTStencilOperationDecrementWrap = 7,
};

struct WMTStencilInfo {
  bool enabled;
  enum WMTStencilOperation depth_stencil_pass_op;
  enum WMTStencilOperation stencil_fail_op;
  enum WMTStencilOperation depth_fail_op;
  enum WMTCompareFunction stencil_compare_function;
  uint8_t write_mask;
  uint8_t read_mask;
};

struct WMTDepthStencilInfo {
  enum WMTCompareFunction depth_compare_function;
  bool depth_write_enabled;
  struct WMTStencilInfo front_stencil;
  struct WMTStencilInfo back_stencil;
};

WINEMETAL_API obj_handle_t MTLDevice_newDepthStencilState(obj_handle_t device, const struct WMTDepthStencilInfo *info);

enum WMTPixelFormat : uint32_t {
  WMTPixelFormatInvalid = 0,
  WMTPixelFormatA8Unorm = 1,
  WMTPixelFormatR8Unorm = 10,
  WMTPixelFormatR8Unorm_sRGB = 11,
  WMTPixelFormatR8Snorm = 12,
  WMTPixelFormatR8Uint = 13,
  WMTPixelFormatR8Sint = 14,
  WMTPixelFormatR16Unorm = 20,
  WMTPixelFormatR16Snorm = 22,
  WMTPixelFormatR16Uint = 23,
  WMTPixelFormatR16Sint = 24,
  WMTPixelFormatR16Float = 25,
  WMTPixelFormatRG8Unorm = 30,
  WMTPixelFormatRG8Unorm_sRGB = 31,
  WMTPixelFormatRG8Snorm = 32,
  WMTPixelFormatRG8Uint = 33,
  WMTPixelFormatRG8Sint = 34,
  WMTPixelFormatB5G6R5Unorm = 40,
  WMTPixelFormatA1BGR5Unorm = 41,
  WMTPixelFormatABGR4Unorm = 42,
  WMTPixelFormatBGR5A1Unorm = 43,
  WMTPixelFormatR32Uint = 53,
  WMTPixelFormatR32Sint = 54,
  WMTPixelFormatR32Float = 55,
  WMTPixelFormatRG16Unorm = 60,
  WMTPixelFormatRG16Snorm = 62,
  WMTPixelFormatRG16Uint = 63,
  WMTPixelFormatRG16Sint = 64,
  WMTPixelFormatRG16Float = 65,
  WMTPixelFormatRGBA8Unorm = 70,
  WMTPixelFormatRGBA8Unorm_sRGB = 71,
  WMTPixelFormatRGBA8Snorm = 72,
  WMTPixelFormatRGBA8Uint = 73,
  WMTPixelFormatRGBA8Sint = 74,
  WMTPixelFormatBGRA8Unorm = 80,
  WMTPixelFormatBGRA8Unorm_sRGB = 81,
  WMTPixelFormatRGB10A2Unorm = 90,
  WMTPixelFormatRGB10A2Uint = 91,
  WMTPixelFormatRG11B10Float = 92,
  WMTPixelFormatRGB9E5Float = 93,
  WMTPixelFormatBGR10A2Unorm = 94,
  WMTPixelFormatBGR10_XR = 554,
  WMTPixelFormatBGR10_XR_sRGB = 555,
  WMTPixelFormatRG32Uint = 103,
  WMTPixelFormatRG32Sint = 104,
  WMTPixelFormatRG32Float = 105,
  WMTPixelFormatRGBA16Unorm = 110,
  WMTPixelFormatRGBA16Snorm = 112,
  WMTPixelFormatRGBA16Uint = 113,
  WMTPixelFormatRGBA16Sint = 114,
  WMTPixelFormatRGBA16Float = 115,
  WMTPixelFormatBGRA10_XR = 552,
  WMTPixelFormatBGRA10_XR_sRGB = 553,
  WMTPixelFormatRGBA32Uint = 123,
  WMTPixelFormatRGBA32Sint = 124,
  WMTPixelFormatRGBA32Float = 125,
  WMTPixelFormatBC1_RGBA = 130,
  WMTPixelFormatBC1_RGBA_sRGB = 131,
  WMTPixelFormatBC2_RGBA = 132,
  WMTPixelFormatBC2_RGBA_sRGB = 133,
  WMTPixelFormatBC3_RGBA = 134,
  WMTPixelFormatBC3_RGBA_sRGB = 135,
  WMTPixelFormatBC4_RUnorm = 140,
  WMTPixelFormatBC4_RSnorm = 141,
  WMTPixelFormatBC5_RGUnorm = 142,
  WMTPixelFormatBC5_RGSnorm = 143,
  WMTPixelFormatBC6H_RGBFloat = 150,
  WMTPixelFormatBC6H_RGBUfloat = 151,
  WMTPixelFormatBC7_RGBAUnorm = 152,
  WMTPixelFormatBC7_RGBAUnorm_sRGB = 153,
  WMTPixelFormatPVRTC_RGB_2BPP = 160,
  WMTPixelFormatPVRTC_RGB_2BPP_sRGB = 161,
  WMTPixelFormatPVRTC_RGB_4BPP = 162,
  WMTPixelFormatPVRTC_RGB_4BPP_sRGB = 163,
  WMTPixelFormatPVRTC_RGBA_2BPP = 164,
  WMTPixelFormatPVRTC_RGBA_2BPP_sRGB = 165,
  WMTPixelFormatPVRTC_RGBA_4BPP = 166,
  WMTPixelFormatPVRTC_RGBA_4BPP_sRGB = 167,
  WMTPixelFormatEAC_R11Unorm = 170,
  WMTPixelFormatEAC_R11Snorm = 172,
  WMTPixelFormatEAC_RG11Unorm = 174,
  WMTPixelFormatEAC_RG11Snorm = 176,
  WMTPixelFormatEAC_RGBA8 = 178,
  WMTPixelFormatEAC_RGBA8_sRGB = 179,
  WMTPixelFormatETC2_RGB8 = 180,
  WMTPixelFormatETC2_RGB8_sRGB = 181,
  WMTPixelFormatETC2_RGB8A1 = 182,
  WMTPixelFormatETC2_RGB8A1_sRGB = 183,
  WMTPixelFormatASTC_4x4_sRGB = 186,
  WMTPixelFormatASTC_5x4_sRGB = 187,
  WMTPixelFormatASTC_5x5_sRGB = 188,
  WMTPixelFormatASTC_6x5_sRGB = 189,
  WMTPixelFormatASTC_6x6_sRGB = 190,
  WMTPixelFormatASTC_8x5_sRGB = 192,
  WMTPixelFormatASTC_8x6_sRGB = 193,
  WMTPixelFormatASTC_8x8_sRGB = 194,
  WMTPixelFormatASTC_10x5_sRGB = 195,
  WMTPixelFormatASTC_10x6_sRGB = 196,
  WMTPixelFormatASTC_10x8_sRGB = 197,
  WMTPixelFormatASTC_10x10_sRGB = 198,
  WMTPixelFormatASTC_12x10_sRGB = 199,
  WMTPixelFormatASTC_12x12_sRGB = 200,
  WMTPixelFormatASTC_4x4_LDR = 204,
  WMTPixelFormatASTC_5x4_LDR = 205,
  WMTPixelFormatASTC_5x5_LDR = 206,
  WMTPixelFormatASTC_6x5_LDR = 207,
  WMTPixelFormatASTC_6x6_LDR = 208,
  WMTPixelFormatASTC_8x5_LDR = 210,
  WMTPixelFormatASTC_8x6_LDR = 211,
  WMTPixelFormatASTC_8x8_LDR = 212,
  WMTPixelFormatASTC_10x5_LDR = 213,
  WMTPixelFormatASTC_10x6_LDR = 214,
  WMTPixelFormatASTC_10x8_LDR = 215,
  WMTPixelFormatASTC_10x10_LDR = 216,
  WMTPixelFormatASTC_12x10_LDR = 217,
  WMTPixelFormatASTC_12x12_LDR = 218,
  WMTPixelFormatASTC_4x4_HDR = 222,
  WMTPixelFormatASTC_5x4_HDR = 223,
  WMTPixelFormatASTC_5x5_HDR = 224,
  WMTPixelFormatASTC_6x5_HDR = 225,
  WMTPixelFormatASTC_6x6_HDR = 226,
  WMTPixelFormatASTC_8x5_HDR = 228,
  WMTPixelFormatASTC_8x6_HDR = 229,
  WMTPixelFormatASTC_8x8_HDR = 230,
  WMTPixelFormatASTC_10x5_HDR = 231,
  WMTPixelFormatASTC_10x6_HDR = 232,
  WMTPixelFormatASTC_10x8_HDR = 233,
  WMTPixelFormatASTC_10x10_HDR = 234,
  WMTPixelFormatASTC_12x10_HDR = 235,
  WMTPixelFormatASTC_12x12_HDR = 236,
  WMTPixelFormatGBGR422 = 240,
  WMTPixelFormatBGRG422 = 241,
  WMTPixelFormatDepth16Unorm = 250,
  WMTPixelFormatDepth32Float = 252,
  WMTPixelFormatStencil8 = 253,
  WMTPixelFormatDepth24Unorm_Stencil8 = 255,
  WMTPixelFormatDepth32Float_Stencil8 = 260,
  WMTPixelFormatX32_Stencil8 = 261,
  WMTPixelFormatX24_Stencil8 = 262,

  /**
   * Extended (swizzled) format
   *
   * NOTE: DO NOT use these formats with a non-default (RGBA) swizzle property in `newTextureView` view descriptor
   * use one of them
   * - an ordinary format with custom swizzle
   * - an extended format with the default rgba swizzle
   * just don't mix them, otherwise it would be really confusing
   */

  WMTPixelFormatCustomSwizzle = 0b111111111111u << 12,

  WMTPixelFormatAlphaIsOne = 0b000000000010u << 12,
  WMTPixelFormatR001Swizzle = 0b000011010010u << 12,
  WMTPixelFormat0R01Swizzle = 0b100101010010u << 12,
  WMTPixelFormatGBARSwizzle = 0b001001001011u << 12,
  WMTPixelFormatRGB1Swizzle = WMTPixelFormatAlphaIsOne,

  WMTPixelFormatBGRX8Unorm = WMTPixelFormatAlphaIsOne | WMTPixelFormatBGRA8Unorm,
  WMTPixelFormatBGRX8Unorm_sRGB = WMTPixelFormatAlphaIsOne | WMTPixelFormatBGRA8Unorm_sRGB,

  WMTPixelFormatR32X8X32 = WMTPixelFormatR001Swizzle | WMTPixelFormatDepth32Float_Stencil8,
  WMTPixelFormatX32G8X32 = WMTPixelFormat0R01Swizzle | WMTPixelFormatX32_Stencil8,

  WMTPixelFormatBGRA4Unorm = WMTPixelFormatGBARSwizzle | WMTPixelFormatABGR4Unorm,
};

#define ORIGINAL_FORMAT(format) ((format) & ~WMTPixelFormatCustomSwizzle)

#define DECODE_FORMAT_COMPONENT_SWIZZLE(format, component, base) ((base + ((format >> (21 - 3 * component)) & 0x7)) % 6)

#define GET_SWIZZLE_RED(format) DECODE_FORMAT_COMPONENT_SWIZZLE(format, 0, 2 /* TextureSwizzleRed */)
#define GET_SWIZZLE_GREEN(format) DECODE_FORMAT_COMPONENT_SWIZZLE(format, 1, 3 /* TextureSwizzleGreen */)
#define GET_SWIZZLE_BLUE(format) DECODE_FORMAT_COMPONENT_SWIZZLE(format, 2, 4 /* TextureSwizzleBlue */)
#define GET_SWIZZLE_ALPHA(format) DECODE_FORMAT_COMPONENT_SWIZZLE(format, 3, 5 /* TextureSwizzleAlpha */)

#define ENCODE_FORMAT_COMPONENT_OFFSET(target, base) (((target) + (6 - base)) % 6)

#define ENCODE_FORMAT_SWIZZLE(base_format, r, g, b, a)                                                                 \
  ((base_format) & 0xfff) | (ENCODE_FORMAT_COMPONENT_OFFSET(r, 2 /* TextureSwizzleRed */) << 21) |                     \
      (ENCODE_FORMAT_COMPONENT_OFFSET(g, 3 /* TextureSwizzleGreen */) << 18) |                                         \
      (ENCODE_FORMAT_COMPONENT_OFFSET(b, 4 /* TextureSwizzleBlue */) << 15) |                                          \
      (ENCODE_FORMAT_COMPONENT_OFFSET(a, 5 /* TextureSwizzleAlpha */) << 12)

enum WMTTextureType : uint32_t {
  WMTTextureType1D = 0,
  WMTTextureType1DArray = 1,
  WMTTextureType2D = 2,
  WMTTextureType2DArray = 3,
  WMTTextureType2DMultisample = 4,
  WMTTextureTypeCube = 5,
  WMTTextureTypeCubeArray = 6,
  WMTTextureType3D = 7,
  WMTTextureType2DMultisampleArray = 8,
  WMTTextureTypeTextureBuffer = 9,
};

enum WMTTextureUsage : uint32_t {
  WMTTextureUsageUnknown = 0,
  WMTTextureUsageShaderRead = 1,
  WMTTextureUsageShaderWrite = 2,
  WMTTextureUsageRenderTarget = 4,
  WMTTextureUsagePixelFormatView = 16,
  WMTTextureUsageShaderAtomic = 32,
};

enum WMTTextureSwizzle : uint8_t {
  WMTTextureSwizzleZero = 0,
  WMTTextureSwizzleOne = 1,
  WMTTextureSwizzleRed = 2,
  WMTTextureSwizzleGreen = 3,
  WMTTextureSwizzleBlue = 4,
  WMTTextureSwizzleAlpha = 5,
};

struct WMTTextureSwizzleChannels {
  enum WMTTextureSwizzle r;
  enum WMTTextureSwizzle g;
  enum WMTTextureSwizzle b;
  enum WMTTextureSwizzle a;
};

struct WMTTextureInfo {
  enum WMTPixelFormat pixel_format;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t array_length;
  enum WMTTextureType type    : 8;
  uint32_t mipmap_level_count : 8;
  uint32_t sample_count       : 8;
  enum WMTTextureUsage usage  : 8;
  enum WMTResourceOptions options;
  uint32_t reserved;
  mach_port_t mach_port; // in/out
  uint64_t gpu_resource_id; // out
};

WINEMETAL_API obj_handle_t MTLDevice_newTexture(obj_handle_t device, struct WMTTextureInfo *info);

WINEMETAL_API obj_handle_t
MTLBuffer_newTexture(obj_handle_t buffer, struct WMTTextureInfo *info, uint64_t offset, uint64_t bytes_per_row);

WINEMETAL_API obj_handle_t MTLTexture_newTextureView(
    obj_handle_t texture, enum WMTPixelFormat format, enum WMTTextureType texture_type, uint16_t level_start,
    uint16_t level_count, uint16_t slice_start, uint16_t slice_count, struct WMTTextureSwizzleChannels swizzle,
    uint64_t *out_gpu_resource_id
);

WINEMETAL_API uint64_t
MTLDevice_minimumLinearTextureAlignmentForPixelFormat(obj_handle_t device, enum WMTPixelFormat format);

enum WMTAttributeFormat : uint32_t {
  WMTAttributeFormatInvalid = 0,
  WMTAttributeFormatUChar2 = 1,
  WMTAttributeFormatUChar3 = 2,
  WMTAttributeFormatUChar4 = 3,
  WMTAttributeFormatChar2 = 4,
  WMTAttributeFormatChar3 = 5,
  WMTAttributeFormatChar4 = 6,
  WMTAttributeFormatUChar2Normalized = 7,
  WMTAttributeFormatUChar3Normalized = 8,
  WMTAttributeFormatUChar4Normalized = 9,
  WMTAttributeFormatChar2Normalized = 10,
  WMTAttributeFormatChar3Normalized = 11,
  WMTAttributeFormatChar4Normalized = 12,
  WMTAttributeFormatUShort2 = 13,
  WMTAttributeFormatUShort3 = 14,
  WMTAttributeFormatUShort4 = 15,
  WMTAttributeFormatShort2 = 16,
  WMTAttributeFormatShort3 = 17,
  WMTAttributeFormatShort4 = 18,
  WMTAttributeFormatUShort2Normalized = 19,
  WMTAttributeFormatUShort3Normalized = 20,
  WMTAttributeFormatUShort4Normalized = 21,
  WMTAttributeFormatShort2Normalized = 22,
  WMTAttributeFormatShort3Normalized = 23,
  WMTAttributeFormatShort4Normalized = 24,
  WMTAttributeFormatHalf2 = 25,
  WMTAttributeFormatHalf3 = 26,
  WMTAttributeFormatHalf4 = 27,
  WMTAttributeFormatFloat = 28,
  WMTAttributeFormatFloat2 = 29,
  WMTAttributeFormatFloat3 = 30,
  WMTAttributeFormatFloat4 = 31,
  WMTAttributeFormatInt = 32,
  WMTAttributeFormatInt2 = 33,
  WMTAttributeFormatInt3 = 34,
  WMTAttributeFormatInt4 = 35,
  WMTAttributeFormatUInt = 36,
  WMTAttributeFormatUInt2 = 37,
  WMTAttributeFormatUInt3 = 38,
  WMTAttributeFormatUInt4 = 39,
  WMTAttributeFormatInt1010102Normalized = 40,
  WMTAttributeFormatUInt1010102Normalized = 41,
  WMTAttributeFormatUChar4Normalized_BGRA = 42,
  WMTAttributeFormatUChar = 45,
  WMTAttributeFormatChar = 46,
  WMTAttributeFormatUCharNormalized = 47,
  WMTAttributeFormatCharNormalized = 48,
  WMTAttributeFormatUShort = 49,
  WMTAttributeFormatShort = 50,
  WMTAttributeFormatUShortNormalized = 51,
  WMTAttributeFormatShortNormalized = 52,
  WMTAttributeFormatHalf = 53,
  WMTAttributeFormatFloatRG11B10 = 54,
  WMTAttributeFormatFloatRGB9E5 = 55,
};

WINEMETAL_API obj_handle_t MTLDevice_newLibrary(obj_handle_t device, obj_handle_t data, obj_handle_t *err_out);

WINEMETAL_API obj_handle_t MTLLibrary_newFunction(obj_handle_t library, const char *name);

WINEMETAL_API uint64_t NSString_lengthOfBytesUsingEncoding(obj_handle_t str, enum WMTStringEncoding encoding);

WINEMETAL_API obj_handle_t NSObject_description(obj_handle_t nserror);

struct WMTComputePipelineInfo {
  obj_handle_t compute_function;
  struct WMTConstMemoryPointer binary_archives_for_lookup;
  obj_handle_t binary_archive_for_serialization;
  uint8_t num_binary_archives_for_lookup;
  bool fail_on_binary_archive_miss;
  bool support_indirect_command_buffers;
  bool tgsize_is_multiple_of_sgwidth;
  uint32_t immutable_buffers;
};

WINEMETAL_API obj_handle_t MTLDevice_newComputePipelineState(
    obj_handle_t device, const struct WMTComputePipelineInfo *info, obj_handle_t *err_out
);

WINEMETAL_API obj_handle_t MTLCommandBuffer_blitCommandEncoder(obj_handle_t cmdbuf);

WINEMETAL_API obj_handle_t MTLCommandBuffer_computeCommandEncoder(obj_handle_t cmdbuf, bool concurrent);

enum WMTLoadAction {
  WMTLoadActionDontCare = 0,
  WMTLoadActionLoad = 1,
  WMTLoadActionClear = 2,
};

enum WMTStoreAction {
  WMTStoreActionDontCare = 0,
  WMTStoreActionStore = 1,
  WMTStoreActionMultisampleResolve = 2,
  WMTStoreActionStoreAndMultisampleResolve = 3,
  WMTStoreActionUnknown = 4,
  WMTStoreActionCustomSampleDepthStore = 5,
};

struct WMTClearColor {
  double r;
  double g;
  double b;
  double a;
};

struct WMTColorAttachmentInfo {
  obj_handle_t texture;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  struct WMTClearColor clear_color;
  obj_handle_t resolve_texture;
  uint16_t resolve_level;
  uint16_t resolve_slice;
  uint32_t resolve_depth_plane;
};

struct WMTDepthAttachmentInfo {
  obj_handle_t texture;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  float clear_depth;
};

struct WMTStencilAttachmentInfo {
  obj_handle_t texture;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  uint8_t clear_stencil;
};

struct WMTRenderPassInfo {
  struct WMTColorAttachmentInfo colors[8];
  struct WMTDepthAttachmentInfo depth;
  struct WMTStencilAttachmentInfo stencil;
  uint8_t default_raster_sample_count;
  uint8_t render_target_array_length;
  uint8_t tile_width;
  uint8_t tile_height;
  uint32_t render_target_height;
  uint32_t render_target_width;
  obj_handle_t visibility_buffer;
};

WINEMETAL_API obj_handle_t MTLCommandBuffer_renderCommandEncoder(obj_handle_t cmdbuf, struct WMTRenderPassInfo *info);

WINEMETAL_API void MTLCommandEncoder_endEncoding(obj_handle_t encoder);

enum WMTTessellationPartitionMode : uint8_t {
  WMTTessellationPartitionModePow2 = 0,
  WMTTessellationPartitionModeInteger = 1,
  WMTTessellationPartitionModeFractionalOdd = 2,
  WMTTessellationPartitionModeFractionalEven = 3,
};

enum WMTTessellationFactorStepFunction : uint8_t {
  WMTTessellationFactorStepFunctionConstant = 0,
  WMTTessellationFactorStepFunctionPerPatch = 1,
  WMTTessellationFactorStepFunctionPerInstance = 2,
  WMTTessellationFactorStepFunctionPerPatchAndPerInstance = 3,
};

enum WMTWinding : uint8_t {
  WMTWindingClockwise = 0,
  WMTWindingCounterClockwise = 1,
};

enum WMTBlendOperation : uint8_t {
  WMTBlendOperationAdd = 0,
  WMTBlendOperationSubtract = 1,
  WMTBlendOperationReverseSubtract = 2,
  WMTBlendOperationMin = 3,
  WMTBlendOperationMax = 4,
};

enum WMTLogicOperation : uint8_t {
  WMTLogicOperationClear = 0,
  WMTLogicOperationSet = 1,
  WMTLogicOperationCopy = 2,
  WMTLogicOperationCopyInverted = 3,
  WMTLogicOperationNoOp = 4,
  WMTLogicOperationInvert = 5,
  WMTLogicOperationAnd = 6,
  WMTLogicOperationNand = 7,
  WMTLogicOperationOr = 8,
  WMTLogicOperationNor = 9,
  WMTLogicOperationXor = 10,
  WMTLogicOperationEquiv = 11,
  WMTLogicOperationAndReverse = 12,
  WMTLogicOperationAndInverted = 13,
  WMTLogicOperationOrReverse = 14,
  WMTLogicOperationOrInverted = 15,
};

enum WMTBlendFactor : uint8_t {
  WMTBlendFactorZero = 0,
  WMTBlendFactorOne = 1,
  WMTBlendFactorSourceColor = 2,
  WMTBlendFactorOneMinusSourceColor = 3,
  WMTBlendFactorSourceAlpha = 4,
  WMTBlendFactorOneMinusSourceAlpha = 5,
  WMTBlendFactorDestinationColor = 6,
  WMTBlendFactorOneMinusDestinationColor = 7,
  WMTBlendFactorDestinationAlpha = 8,
  WMTBlendFactorOneMinusDestinationAlpha = 9,
  WMTBlendFactorSourceAlphaSaturated = 10,
  WMTBlendFactorBlendColor = 11,
  WMTBlendFactorOneMinusBlendColor = 12,
  WMTBlendFactorBlendAlpha = 13,
  WMTBlendFactorOneMinusBlendAlpha = 14,
  WMTBlendFactorSource1Color = 15,
  WMTBlendFactorOneMinusSource1Color = 16,
  WMTBlendFactorSource1Alpha = 17,
  WMTBlendFactorOneMinusSource1Alpha = 18,
};

enum WMTColorWriteMask : uint8_t {
  WMTColorWriteMaskNone = 0,
  WMTColorWriteMaskRed = 8,
  WMTColorWriteMaskGreen = 4,
  WMTColorWriteMaskBlue = 2,
  WMTColorWriteMaskAlpha = 1,
  WMTColorWriteMaskAll = 15,
};

struct WMTColorAttachmentBlendInfo {
  enum WMTPixelFormat pixel_format;
  enum WMTBlendOperation rgb_blend_operation;
  enum WMTBlendOperation alpha_blend_operation;
  enum WMTBlendFactor src_rgb_blend_factor;
  enum WMTBlendFactor dst_rgb_blend_factor;
  enum WMTBlendFactor src_alpha_blend_factor;
  enum WMTBlendFactor dst_alpha_blend_factor;
  uint8_t write_mask;
  bool blending_enabled;
};

enum WMTPrimitiveTopologyClass {
  WMTPrimitiveTopologyClassUnspecified = 0,
  WMTPrimitiveTopologyClassPoint = 1,
  WMTPrimitiveTopologyClassLine = 2,
  WMTPrimitiveTopologyClassTriangle = 3,
};

struct WMTRenderPipelineBlendInfo {
  struct WMTColorAttachmentBlendInfo colors[8];
  bool alpha_to_coverage_enabled;
  bool logic_operation_enabled;
  enum WMTLogicOperation logic_operation;
};

struct WMTRenderPipelineInfo {
  struct WMTColorAttachmentBlendInfo colors[8];
  bool alpha_to_coverage_enabled;
  bool logic_operation_enabled;
  enum WMTLogicOperation logic_operation;
  bool rasterization_enabled;
  uint8_t raster_sample_count;
  enum WMTPixelFormat depth_pixel_format;
  enum WMTPixelFormat stencil_pixel_format;
  obj_handle_t vertex_function;
  obj_handle_t fragment_function;
  uint32_t immutable_vertex_buffers;
  uint32_t immutable_fragment_buffers;
  enum WMTPrimitiveTopologyClass input_primitive_topology;
  enum WMTTessellationPartitionMode tessellation_partition_mode;
  uint8_t max_tessellation_factor;
  enum WMTWinding tessellation_output_winding_order;
  enum WMTTessellationFactorStepFunction tessellation_factor_step;
  obj_handle_t binary_archive_for_serialization;
  struct WMTConstMemoryPointer binary_archives_for_lookup;
  uint8_t num_binary_archives_for_lookup;
  bool fail_on_binary_archive_miss;
  bool support_indirect_command_buffers;
  uint8_t padding[5];
};

struct WMTMeshRenderPipelineInfo {
  struct WMTColorAttachmentBlendInfo colors[8];
  bool alpha_to_coverage_enabled;
  bool logic_operation_enabled;
  enum WMTLogicOperation logic_operation;
  bool rasterization_enabled;
  uint8_t raster_sample_count;
  enum WMTPixelFormat depth_pixel_format;
  enum WMTPixelFormat stencil_pixel_format;
  obj_handle_t object_function;
  obj_handle_t mesh_function;
  obj_handle_t fragment_function;
  uint32_t immutable_object_buffers;
  uint32_t immutable_mesh_buffers;
  uint32_t immutable_fragment_buffers;
  uint16_t payload_memory_length;
  bool mesh_tgsize_is_multiple_of_sgwidth;
  bool object_tgsize_is_multiple_of_sgwidth;
  obj_handle_t binary_archive_for_serialization;
  struct WMTConstMemoryPointer binary_archives_for_lookup;
  uint8_t num_binary_archives_for_lookup;
  bool fail_on_binary_archive_miss;
  bool support_indirect_command_buffers;
  uint8_t padding[5];
};

WINEMETAL_API obj_handle_t
MTLDevice_newRenderPipelineState(obj_handle_t device, const struct WMTRenderPipelineInfo *info, obj_handle_t *err_out);

WINEMETAL_API obj_handle_t MTLDevice_newMeshRenderPipelineState(
    obj_handle_t device, const struct WMTMeshRenderPipelineInfo *info, obj_handle_t *err_out
);

struct WMTSize {
  uint64_t width;
  uint64_t height;
  uint64_t depth;
};

struct WMTOrigin {
  uint64_t x;
  uint64_t y;
  uint64_t z;
};

enum WMTBlitCommandType : uint16_t {
  WMTBlitCommandNop,
  WMTBlitCommandCopyFromBufferToBuffer,
  WMTBlitCommandCopyFromBufferToTexture,
  WMTBlitCommandCopyFromTextureToBuffer,
  WMTBlitCommandCopyFromTextureToTexture,
  WMTBlitCommandGenerateMipmaps,
  WMTBlitCommandWaitForFence,
  WMTBlitCommandUpdateFence,
  WMTBlitCommandFillBuffer,
  WMTBlitCommandResolveCounters,
  WMTBlitCommandCopyFromBufferToTextureWithBlitOption,
  WMTBlitCommandCopyFromTextureToBufferWithBlitOption,
  WMTBlitCommandResetCommandsInBuffer,
  WMTBlitCommandCopyTexture,
};

enum WMTBlitOption : uint16_t {
  WMTBlitOptionNone = 0,
  WMTBlitOptionDepthFromDepthStencil = 1,
  WMTBlitOptionStencilFromDepthStencil = 2,
  WMTBlitOptionRowLinearPVRTC = 4,
};

struct wmtcmd_base {
  uint16_t type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
};

struct wmtcmd_blit_nop {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
};

struct wmtcmd_blit_copy_from_buffer_to_buffer {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t src;
  uint64_t src_offset;
  obj_handle_t dst;
  uint64_t dst_offset;
  uint64_t copy_length;
};

struct wmtcmd_blit_copy_from_buffer_to_texture {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t src;
  uint64_t src_offset;
  uint32_t bytes_per_row;
  uint32_t bytes_per_image;
  struct WMTSize size;
  obj_handle_t dst;
  uint32_t slice;
  uint32_t level;
  struct WMTOrigin origin;
};

struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t src;
  uint64_t src_offset;
  uint32_t bytes_per_row;
  uint32_t bytes_per_image;
  struct WMTSize size;
  obj_handle_t dst;
  uint32_t slice;
  uint16_t level;
  uint16_t options;
  struct WMTOrigin origin;
};

struct wmtcmd_blit_copy_from_texture_to_texture {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t src;
  uint32_t src_slice;
  uint32_t src_level;
  struct WMTOrigin src_origin;
  struct WMTSize src_size;
  obj_handle_t dst;
  uint32_t dst_slice;
  uint32_t dst_level;
  struct WMTOrigin dst_origin;
};

struct wmtcmd_blit_copy_from_texture_to_buffer {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t src;
  uint32_t slice;
  uint32_t level;
  struct WMTOrigin origin;
  struct WMTSize size;
  obj_handle_t dst;
  uint64_t offset;
  uint32_t bytes_per_row;
  uint32_t bytes_per_image;
};

struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t src;
  uint32_t slice;
  uint16_t level;
  uint16_t options;
  struct WMTOrigin origin;
  struct WMTSize size;
  obj_handle_t dst;
  uint64_t offset;
  uint32_t bytes_per_row;
  uint32_t bytes_per_image;
};

struct wmtcmd_blit_generate_mipmaps {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t texture;
};

struct wmtcmd_blit_fence_op {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t fence;
};

struct wmtcmd_blit_fillbuffer {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t buffer;
  uint64_t offset;
  uint64_t length;
  uint8_t value;
};

struct wmtcmd_blit_resolvecounters {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t sample_buffer;
  uint32_t start;
  uint32_t len;
  obj_handle_t dst_buffer;
  uint64_t dst_offset;
};

struct wmtcmd_blit_resetcommands {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t indirect_command_buffer;
  uint32_t location;
  uint32_t length;
};

struct wmtcmd_blit_copy_texture {
  enum WMTBlitCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t src;
  obj_handle_t dst;
};

WINEMETAL_API void MTLBlitCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head);

enum WMTComputeCommandType : uint16_t {
  WMTComputeCommandNop,
  WMTComputeCommandDispatch,
  WMTComputeCommandDispatchIndirect,
  WMTComputeCommandSetPSO,
  WMTComputeCommandSetBuffer,
  WMTComputeCommandSetBufferOffset,
  WMTComputeCommandUseResource,
  WMTComputeCommandSetBytes,
  WMTComputeCommandSetTexture,
  WMTComputeCommandDispatchThreads,
  WMTComputeCommandWaitForFence,
  WMTComputeCommandUpdateFence,
  WMTComputeCommandMemoryBarrier,
  WMTComputeCommandExecuteCommandsInBuffer,
};

struct wmtcmd_compute_nop {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
};

struct wmtcmd_compute_dispatch {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  struct WMTSize size;
};

struct wmtcmd_compute_dispatch_indirect {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t indirect_args_buffer;
  uint64_t indirect_args_offset;
};

struct wmtcmd_compute_setpso {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t pso;
  struct WMTSize threadgroup_size;
};

struct wmtcmd_compute_setbuffer {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t buffer;
  uint64_t offset;
  uint8_t index;
};

struct wmtcmd_compute_setbufferoffset {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint64_t offset;
  uint8_t index;
};

enum WMTResourceUsage {
  WMTResourceUsageRead = 1,
  WMTResourceUsageWrite = 2,
  WMTResourceUsageSample = 4,
};

struct wmtcmd_compute_useresource {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t resource;
  enum WMTResourceUsage usage;
};

struct wmtcmd_compute_setbytes {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  struct WMTMemoryPointer bytes;
  uint64_t length;
  uint8_t index;
};

struct wmtcmd_compute_settexture {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t texture;
  uint8_t index;
};

struct wmtcmd_compute_fence_op {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t fence;
};

enum WMTBarrierScope : uint8_t {
  WMTBarrierScopeBuffers = 1,
  WMTBarrierScopeTextures = 2,
  WMTBarrierScopeRenderTargets = 4,
};

struct wmtcmd_compute_memory_barrier {
enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  enum WMTBarrierScope scope;
};

struct wmtcmd_compute_executecommands {
  enum WMTComputeCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t indirect_command_buffer;
  uint32_t location;
  uint32_t length;
};

WINEMETAL_API void MTLComputeCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head);

enum WMTRenderCommandType : uint16_t {
  WMTRenderCommandNop,
  WMTRenderCommandUseResource,
  WMTRenderCommandSetVertexBuffer,
  WMTRenderCommandSetVertexBufferOffset,
  WMTRenderCommandSetFragmentBuffer,
  WMTRenderCommandSetFragmentBufferOffset,
  WMTRenderCommandSetMeshBuffer,
  WMTRenderCommandSetMeshBufferOffset,
  WMTRenderCommandSetObjectBuffer,
  WMTRenderCommandSetObjectBufferOffset,
  WMTRenderCommandSetFragmentTexture,
  WMTRenderCommandSetFragmentBytes,
  WMTRenderCommandSetRasterizerState,
  WMTRenderCommandSetViewports,
  WMTRenderCommandSetScissorRects,
  WMTRenderCommandSetPSO,
  /**
  \deprecated use \ref WMTRenderCommandSetDepthStencilState
  */
  WMTRenderCommandSetDSSO,
  WMTRenderCommandSetBlendFactorAndStencilRef,
  WMTRenderCommandSetVisibilityMode,
  WMTRenderCommandDraw,
  WMTRenderCommandDrawIndexed,
  WMTRenderCommandDrawIndirect,
  WMTRenderCommandDrawIndexedIndirect,
  WMTRenderCommandDrawMeshThreadgroups,
  WMTRenderCommandDrawMeshThreadgroupsIndirect,
  WMTRenderCommandMemoryBarrier,
  Unused0,
  Unused1,
  Unused2,
  WMTRenderCommandDXMTGeometryDraw,
  WMTRenderCommandDXMTGeometryDrawIndexed,
  WMTRenderCommandDXMTGeometryDrawIndirect,
  WMTRenderCommandDXMTGeometryDrawIndexedIndirect,
  WMTRenderCommandWaitForFence,
  WMTRenderCommandUpdateFence,
  WMTRenderCommandSetViewport,
  WMTRenderCommandSetScissorRect,
  WMTRenderCommandDXMTTessellationMeshDraw,
  WMTRenderCommandDXMTTessellationMeshDrawIndexed,
  WMTRenderCommandDXMTTessellationMeshDrawIndirect,
  WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect,
  WMTRenderCommandDispatchThreadsPerTile,
  WMTRenderCommandExecuteCommandsInBuffer,
  WMTRenderCommandSetBlendFactor,
  WMTRenderCommandSetStencilRef,
  WMTRenderCommandSetDepthStencilState,
};

struct wmtcmd_render_nop {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
};

enum WMTRenderStages : uint8_t {
  WMTRenderStageVertex = 1,
  WMTRenderStageFragment = 2,
  WMTRenderStageTile = 4,
  WMTRenderStageObject = 8,
  WMTRenderStageMesh = 16,
  WMTRenderStagePreRaster = WMTRenderStageVertex | WMTRenderStageObject | WMTRenderStageMesh,
};

struct wmtcmd_render_useresource {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t resource;
  enum WMTResourceUsage usage;
  enum WMTRenderStages stages;
};

struct wmtcmd_render_setbuffer {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t buffer;
  uint64_t offset;
  uint8_t index;
};

struct wmtcmd_render_setbytes {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  struct WMTMemoryPointer bytes;
  uint64_t length;
  uint8_t index;
};

struct wmtcmd_render_settexture {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t texture;
  uint8_t index;
};

struct wmtcmd_render_setbufferoffset {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint64_t offset;
  uint8_t index;
};

enum WMTTriangleFillMode : uint8_t {
  WMTTriangleFillModeFill = 0,
  WMTTriangleFillModeLines = 1,
};

enum WMTCullMode : uint8_t {
  WMTCullModeNone = 0,
  WMTCullModeFront = 1,
  WMTCullModeBack = 2,
};

enum WMTDepthClipMode : uint8_t {
  WMTDepthClipModeClip = 0,
  WMTDepthClipModeClamp = 1,
};

struct wmtcmd_render_setrasterizerstate {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  enum WMTTriangleFillMode fill_mode;
  enum WMTCullMode cull_mode;
  enum WMTDepthClipMode depth_clip_mode;
  enum WMTWinding winding;
  float depth_bias;
  float scole_scale;
  float depth_bias_clamp;
};

struct wmtcmd_render_setpso {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t pso;
};

enum WMTVisibilityResultMode : uint8_t {
  WMTVisibilityResultModeDisabled = 0,
  WMTVisibilityResultModeBoolean = 1,
  WMTVisibilityResultModeCounting = 2,
};

struct wmtcmd_render_setvisibilitymode {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint64_t offset;
  enum WMTVisibilityResultMode mode;
};

enum WMTIndexType : uint8_t {
  WMTIndexTypeUInt16 = 0,
  WMTIndexTypeUInt32 = 1,
};

enum WMTPrimitiveType : uint8_t {
  WMTPrimitiveTypePoint = 0,
  WMTPrimitiveTypeLine = 1,
  WMTPrimitiveTypeLineStrip = 2,
  WMTPrimitiveTypeTriangle = 3,
  WMTPrimitiveTypeTriangleStrip = 4,
};

struct wmtcmd_render_draw {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  enum WMTPrimitiveType primitive_type;
  uint64_t vertex_start;
  uint64_t vertex_count;
  uint32_t instance_count;
  uint32_t base_instance;
};

struct wmtcmd_render_draw_indirect {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  enum WMTPrimitiveType primitive_type;
  obj_handle_t indirect_args_buffer;
  uint64_t indirect_args_offset;
};

struct wmtcmd_render_draw_indexed {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  enum WMTPrimitiveType primitive_type;
  enum WMTIndexType index_type;
  uint64_t index_count;
  obj_handle_t index_buffer;
  uint64_t index_buffer_offset;
  uint32_t instance_count;
  int32_t base_vertex;
  uint32_t base_instance;
};

struct wmtcmd_render_draw_indexed_indirect {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  enum WMTPrimitiveType primitive_type;
  enum WMTIndexType index_type;
  obj_handle_t index_buffer;
  uint64_t index_buffer_offset;
  obj_handle_t indirect_args_buffer;
  uint64_t indirect_args_offset;
};

struct wmtcmd_render_draw_meshthreadgroups {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  struct WMTSize threadgroup_per_grid;
  struct WMTSize object_threadgroup_size;
  struct WMTSize mesh_threadgroup_size;
};
struct wmtcmd_render_draw_meshthreadgroups_indirect {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t indirect_args_buffer;
  uint64_t indirect_args_offset;
  struct WMTSize object_threadgroup_size;
  struct WMTSize mesh_threadgroup_size;
};

struct wmtcmd_render_memory_barrier {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  enum WMTBarrierScope scope;
  enum WMTRenderStages stages_before;
  enum WMTRenderStages stages_after;
};

struct WMTViewport {
  double originX;
  double originY;
  double width;
  double height;
  double znear;
  double zfar;
};

struct wmtcmd_render_setviewports {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  struct WMTMemoryPointer viewports;
  uint8_t viewport_count;
};

struct wmtcmd_render_setviewport {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  struct WMTViewport viewport;
};

struct WMTScissorRect {
  uint64_t x;
  uint64_t y;
  uint64_t width;
  uint64_t height;
};

struct wmtcmd_render_setscissorrects {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  struct WMTMemoryPointer scissor_rects;
  uint8_t rect_count;
};

struct wmtcmd_render_setscissorrect {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  struct WMTScissorRect scissor_rect;
};

struct wmtcmd_render_setdsso {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t dsso;
  uint8_t stencil_ref;
};

struct wmtcmd_render_setdepthstencilstate {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t depth_stencil_state;
};

struct wmtcmd_render_setblendcolor {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  float red;
  float green;
  float blue;
  float alpha;
  uint8_t stencil_ref;
};

struct wmtcmd_render_setstencilref {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint8_t stencil_ref;
};

struct wmtcmd_render_dxmt_geometry_draw {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint64_t draw_arguments_offset;
  uint32_t warp_count;
  uint32_t instance_count;
  uint32_t vertex_per_warp;
};

struct wmtcmd_render_dxmt_geometry_draw_indexed {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint64_t draw_arguments_offset;
  obj_handle_t index_buffer;
  uint64_t index_buffer_offset;
  uint32_t warp_count;
  uint32_t instance_count;
  uint32_t vertex_per_warp;
};

struct wmtcmd_render_dxmt_geometry_draw_indirect {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t imm_draw_arguments;
  obj_handle_t indirect_args_buffer;
  uint64_t indirect_args_offset;
  obj_handle_t dispatch_args_buffer;
  uint64_t dispatch_args_offset;
  uint32_t vertex_per_warp;
};

struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t index_buffer;
  uint64_t index_buffer_offset;
  obj_handle_t imm_draw_arguments;
  obj_handle_t indirect_args_buffer;
  uint64_t indirect_args_offset;
  obj_handle_t dispatch_args_buffer;
  uint64_t dispatch_args_offset;
  uint32_t vertex_per_warp;
};

struct wmtcmd_render_fence_op {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t fence;
  enum WMTRenderStages stages;
};

struct wmtcmd_render_dxmt_tessellation_mesh_draw {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint64_t draw_arguments_offset;
  uint32_t instance_count;
  uint32_t threads_per_patch;
  uint32_t patch_per_group;
  uint32_t patch_per_mesh_instance;
};

struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint64_t draw_arguments_offset;
  obj_handle_t index_buffer;
  uint64_t index_buffer_offset;
  uint32_t instance_count;
  uint32_t threads_per_patch;
  uint32_t patch_per_group;
  uint32_t patch_per_mesh_instance;
};

struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t imm_draw_arguments;
  obj_handle_t indirect_args_buffer;
  uint64_t indirect_args_offset;
  obj_handle_t dispatch_args_buffer;
  uint64_t dispatch_args_offset;
  uint32_t threads_per_patch;
  uint32_t patch_per_group;
};

struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t imm_draw_arguments;
  obj_handle_t indirect_args_buffer;
  uint64_t indirect_args_offset;
  obj_handle_t dispatch_args_buffer;
  uint64_t dispatch_args_offset;
  obj_handle_t index_buffer;
  uint64_t index_buffer_offset;
  uint32_t threads_per_patch;
  uint32_t patch_per_group;
};

struct wmtcmd_render_dispatch_threads_per_tile {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  uint32_t width;
  uint32_t height;
};

struct wmtcmd_render_executecommands {
  enum WMTRenderCommandType type;
  uint16_t reserved[3];
  struct WMTMemoryPointer next;
  obj_handle_t indirect_command_buffer;
  uint32_t location;
  uint32_t length;
};

WINEMETAL_API void MTLRenderCommandEncoder_encodeCommands(obj_handle_t encoder, const struct wmtcmd_base *cmd_head);

WINEMETAL_API enum WMTPixelFormat MTLTexture_pixelFormat(obj_handle_t texture);
WINEMETAL_API uint64_t MTLTexture_width(obj_handle_t texture);
WINEMETAL_API uint64_t MTLTexture_height(obj_handle_t texture);
WINEMETAL_API uint64_t MTLTexture_depth(obj_handle_t texture);
WINEMETAL_API uint64_t MTLTexture_arrayLength(obj_handle_t texture);
WINEMETAL_API uint64_t MTLTexture_mipmapLevelCount(obj_handle_t texture);
WINEMETAL_API void MTLTexture_replaceRegion(
    obj_handle_t texture, struct WMTOrigin origin, struct WMTSize size, uint64_t level, uint64_t slice,
    struct WMTMemoryPointer data, uint64_t bytes_per_row, uint64_t bytes_per_image
);

WINEMETAL_API void MTLBuffer_didModifyRange(obj_handle_t buffer, uint64_t start, uint64_t length);

WINEMETAL_API void MTLCommandBuffer_presentDrawable(obj_handle_t cmdbuf, obj_handle_t drawable);

WINEMETAL_API void
MTLCommandBuffer_presentDrawableAfterMinimumDuration(obj_handle_t cmdbuf, obj_handle_t drawable, double after);

/* gw2: presentDrawable:atTime: CACurrentMediaTime() + interval, taken when this is called. The
   deadline lets macOS raise the GPU clock to meet it; a plain present carries none. */
WINEMETAL_API void
MTLCommandBuffer_presentDrawableWithDeadline(obj_handle_t cmdbuf, obj_handle_t drawable, double interval);

/* gw2 (#7): one drawable's presentation, for the PRESENT statistics section. Times are ns on
   CACurrentMediaTime's clock (the one GPUStartTime/GPUEndTime use); presented_ns 0 = dropped. */
struct WMTPresentedRecord {
  uint64_t frame;
  uint64_t encode_ns;
  uint64_t presented_ns;
};
#define WMT_PRESENTED_BATCH 8

/* gw2 (#7): unless drawable is 0, adds a presented handler to it that records frame, the time of
   this call (the encode time) and the drawable's presentedTime in a bounded ring on the unix side;
   call it right before the drawable's present. Then moves up to min(capacity, WMT_PRESENTED_BATCH)
   finished records into out and returns how many; *lost gets the records the full ring dropped
   since the last drain. Never waits: while a handler holds the ring, nothing is drained. */
WINEMETAL_API uint32_t MTLDrawable_trackPresented(
    obj_handle_t drawable, uint64_t frame, struct WMTPresentedRecord *out, uint32_t capacity, uint32_t *lost
);

enum WMTGPUFamily {
  WMTGPUFamilyApple1 = 1001,
  WMTGPUFamilyApple2 = 1002,
  WMTGPUFamilyApple3 = 1003,
  WMTGPUFamilyApple4 = 1004,
  WMTGPUFamilyApple5 = 1005,
  WMTGPUFamilyApple6 = 1006,
  WMTGPUFamilyApple7 = 1007,
  WMTGPUFamilyApple8 = 1008,
  WMTGPUFamilyApple9 = 1009,
  WMTGPUFamilyMac1 = 2001,
  WMTGPUFamilyMac2 = 2002,
  WMTGPUFamilyCommon1 = 3001,
  WMTGPUFamilyCommon2 = 3002,
  WMTGPUFamilyCommon3 = 3003,
  WMTGPUFamilyMacCatalyst1 = 4001,
  WMTGPUFamilyMacCatalyst2 = 4002,
  WMTGPUFamilyMetal3 = 5001,
  WMTGPUFamilyMetal4 = 5002, // gw2 (#32)
};

WINEMETAL_API bool MTLDevice_supportsFamily(obj_handle_t device, enum WMTGPUFamily gpu_family);

WINEMETAL_API bool MTLDevice_supportsBCTextureCompression(obj_handle_t device);

WINEMETAL_API bool MTLDevice_supportsTextureSampleCount(obj_handle_t device, uint8_t sample_count);

WINEMETAL_API bool MTLDevice_hasUnifiedMemory(obj_handle_t device);

enum WMTCaptureDestination : uint8_t {
  WMTCaptureDestinationDeveloperTools = 1,
  WMTCaptureDestinationGPUTraceDocument = 2,
};

struct WMTCaptureInfo {
  obj_handle_t capture_object;
  struct WMTConstMemoryPointer output_url;
  enum WMTCaptureDestination destination;
};

WINEMETAL_API obj_handle_t MTLCaptureManager_sharedCaptureManager();

WINEMETAL_API bool MTLCaptureManager_startCapture(obj_handle_t mgr, struct WMTCaptureInfo *info);

WINEMETAL_API void MTLCaptureManager_stopCapture(obj_handle_t mgr);

struct WMTFXTemporalScalerInfo {
  enum WMTPixelFormat color_format;
  enum WMTPixelFormat output_format;
  enum WMTPixelFormat depth_format;
  enum WMTPixelFormat motion_format;
  uint32_t input_width;
  uint32_t input_height;
  uint32_t output_width;
  uint32_t output_height;
  float input_content_min_scale;
  float input_content_max_scale;
  bool auto_exposure;
  bool input_content_properties_enabled;
  bool requires_synchronous_initialization;
};

struct WMTFXSpatialScalerInfo {
  enum WMTPixelFormat color_format;
  enum WMTPixelFormat output_format;
  uint32_t input_width;
  uint32_t input_height;
  uint32_t output_width;
  uint32_t output_height;
};

WINEMETAL_API obj_handle_t MTLDevice_newTemporalScaler(obj_handle_t device, const struct WMTFXTemporalScalerInfo *info);

WINEMETAL_API obj_handle_t MTLDevice_newSpatialScaler(obj_handle_t device, const struct WMTFXSpatialScalerInfo *info);

struct WMTFXTemporalScalerProps {
  uint32_t input_content_width;
  uint32_t input_content_height;
  bool reset;
  bool depth_reversed;
  float motion_vector_scale_x;
  float motion_vector_scale_y;
  float jitter_offset_x;
  float jitter_offset_y;
  float pre_exposure;
};

WINEMETAL_API void MTLCommandBuffer_encodeTemporalScale(
    obj_handle_t cmdbuf, obj_handle_t scaler, obj_handle_t color, obj_handle_t output, obj_handle_t depth,
    obj_handle_t motion, obj_handle_t exposure, obj_handle_t fence, const struct WMTFXTemporalScalerProps *props
);

WINEMETAL_API void MTLCommandBuffer_encodeSpatialScale(
    obj_handle_t cmdbuf, obj_handle_t scaler, obj_handle_t color, obj_handle_t output, obj_handle_t fence
);

WINEMETAL_API obj_handle_t NSString_string(const char *data, enum WMTStringEncoding encoding);

WINEMETAL_API obj_handle_t NSString_alloc_init(const char *data, enum WMTStringEncoding encoding);

WINEMETAL_API obj_handle_t DeveloperHUDProperties_instance();

WINEMETAL_API bool DeveloperHUDProperties_addLabel(obj_handle_t obj, obj_handle_t label, obj_handle_t after);

WINEMETAL_API void DeveloperHUDProperties_updateLabel(obj_handle_t obj, obj_handle_t label, obj_handle_t value);

WINEMETAL_API void DeveloperHUDProperties_remove(obj_handle_t obj, obj_handle_t label);

WINEMETAL_API obj_handle_t MetalDrawable_texture(obj_handle_t drawable);

WINEMETAL_API obj_handle_t MetalLayer_nextDrawable(obj_handle_t layer);

WINEMETAL_API bool MTLDevice_supportsFXSpatialScaler(obj_handle_t device);

WINEMETAL_API bool MTLDevice_supportsFXTemporalScaler(obj_handle_t device);

struct WMTLayerProps {
  obj_handle_t device;
  double contents_scale;
  double drawable_width;
  double drawable_height;
  bool opaque;
  bool display_sync_enabled;
  bool framebuffer_only;
  enum WMTPixelFormat pixel_format;
};

WINEMETAL_API void MetalLayer_setProps(obj_handle_t layer, const struct WMTLayerProps *props);

WINEMETAL_API void MetalLayer_getProps(obj_handle_t layer, struct WMTLayerProps *props);

WINEMETAL_API obj_handle_t CreateMetalViewFromHWND(intptr_t hwnd, obj_handle_t device, obj_handle_t *layer);

WINEMETAL_API void ReleaseMetalView(obj_handle_t view);

/* gw2 (#1): the visibility of the window that holds a view from CreateMetalViewFromHWND, for the
   background throttle's occlusion policy. The unix side caches it per view and refreshes it on the
   main thread without waiting, so a call never blocks and returns the state of the previous
   refresh: 0 (no WMTViewVisibilityKnown) until the first one has run. */
enum WMTViewVisibility {
  WMTViewVisibilityKnown = 1,
  WMTViewVisibilityVisible = 2,       /* occlusionState has NSWindowOcclusionStateVisible */
  WMTViewVisibilityMiniaturized = 4,  /* isMiniaturized */
  WMTViewVisibilityOnActiveSpace = 8, /* isOnActiveSpace */
  WMTViewVisibilityNoWindow = 16,     /* the view isn't in a window */
};

WINEMETAL_API uint64_t MetalView_visibility(obj_handle_t view);

WINEMETAL_API void MTLCommandEncoder_setLabel(obj_handle_t encoder, obj_handle_t label);

WINEMETAL_API void MTLDevice_setShouldMaximizeConcurrentCompilation(obj_handle_t device, bool value);

WINEMETAL_API obj_handle_t MTLCommandBuffer_error(obj_handle_t cmdbuf);

WINEMETAL_API obj_handle_t MTLCommandBuffer_logs(obj_handle_t cmdbuf);

WINEMETAL_API uint64_t
MTLLogContainer_enumerate(obj_handle_t logs, uint64_t start, uint64_t buffer_size, obj_handle_t *buffer);

enum WMTColorSpace : uint64_t {
  WMTColorSpaceSRGB = 0b000,
  WMTColorSpaceSRGBLinear = 0b001,
  WMTColorSpaceBT2020 = 0b010,
  WMTColorSpaceHDR_PQ = 0b100,
  WMTColorSpaceHDR10 = WMTColorSpaceHDR_PQ,
  WMTColorSpaceHDR_scRGB = 0b101,
  WMTColorSpaceInvalid = 0xFFFFFFFFFFFFFFFF,
};

#if !defined(WMT_COLORSPACE_IS_HDR)
#define WMT_COLORSPACE_IS_HDR(x) x & 0b100
#endif

WINEMETAL_API bool CGColorSpace_checkColorSpaceSupported(enum WMTColorSpace colorspace);

WINEMETAL_API bool MetalLayer_setColorSpace(obj_handle_t layer, enum WMTColorSpace colorspace);

WINEMETAL_API uint32_t WMTGetPrimaryDisplayId();

WINEMETAL_API uint32_t WMTGetSecondaryDisplayId();

struct WMTDisplayDescription {
  float red_primaries[2];
  float blue_primaries[2];
  float green_primaries[2];
  float white_points[2];
  float maximum_edr_color_component_value;
  float maximum_potential_edr_color_component_value;
  float maximum_reference_edr_color_component_value;
};

WINEMETAL_API void WMTGetDisplayDescription(uint32_t display_id, struct WMTDisplayDescription *desc);

struct WMTEDRValue {
  float maximum_edr_color_component_value;
  float maximum_potential_edr_color_component_value;
};

WINEMETAL_API void MetalLayer_getEDRValue(obj_handle_t layer, struct WMTEDRValue *value);

enum WMTDataType : uint16_t {
  WMTDataTypeNone = 0,

  WMTDataTypeStruct = 1,
  WMTDataTypeArray = 2,

  WMTDataTypeFloat = 3,
  WMTDataTypeFloat2 = 4,
  WMTDataTypeFloat3 = 5,
  WMTDataTypeFloat4 = 6,

  WMTDataTypeFloat2x2 = 7,
  WMTDataTypeFloat2x3 = 8,
  WMTDataTypeFloat2x4 = 9,

  WMTDataTypeFloat3x2 = 10,
  WMTDataTypeFloat3x3 = 11,
  WMTDataTypeFloat3x4 = 12,

  WMTDataTypeFloat4x2 = 13,
  WMTDataTypeFloat4x3 = 14,
  WMTDataTypeFloat4x4 = 15,

  WMTDataTypeHalf = 16,
  WMTDataTypeHalf2 = 17,
  WMTDataTypeHalf3 = 18,
  WMTDataTypeHalf4 = 19,

  WMTDataTypeHalf2x2 = 20,
  WMTDataTypeHalf2x3 = 21,
  WMTDataTypeHalf2x4 = 22,

  WMTDataTypeHalf3x2 = 23,
  WMTDataTypeHalf3x3 = 24,
  WMTDataTypeHalf3x4 = 25,

  WMTDataTypeHalf4x2 = 26,
  WMTDataTypeHalf4x3 = 27,
  WMTDataTypeHalf4x4 = 28,

  WMTDataTypeInt = 29,
  WMTDataTypeInt2 = 30,
  WMTDataTypeInt3 = 31,
  WMTDataTypeInt4 = 32,

  WMTDataTypeUInt = 33,
  WMTDataTypeUInt2 = 34,
  WMTDataTypeUInt3 = 35,
  WMTDataTypeUInt4 = 36,

  WMTDataTypeShort = 37,
  WMTDataTypeShort2 = 38,
  WMTDataTypeShort3 = 39,
  WMTDataTypeShort4 = 40,

  WMTDataTypeUShort = 41,
  WMTDataTypeUShort2 = 42,
  WMTDataTypeUShort3 = 43,
  WMTDataTypeUShort4 = 44,

  WMTDataTypeChar = 45,
  WMTDataTypeChar2 = 46,
  WMTDataTypeChar3 = 47,
  WMTDataTypeChar4 = 48,

  WMTDataTypeUChar = 49,
  WMTDataTypeUChar2 = 50,
  WMTDataTypeUChar3 = 51,
  WMTDataTypeUChar4 = 52,

  WMTDataTypeBool = 53,
  WMTDataTypeBool2 = 54,
  WMTDataTypeBool3 = 55,
  WMTDataTypeBool4 = 56,
};

struct WMTFunctionConstant {
  struct WMTConstMemoryPointer data;
  enum WMTDataType type;
  uint16_t index;
  uint32_t reserved;
};

WINEMETAL_API obj_handle_t MTLLibrary_newFunctionWithConstants(
    obj_handle_t library, const char *name, const struct WMTFunctionConstant *constants, uint32_t num_constants,
    obj_handle_t *err_out
);

struct WMTHDRMetadata {
  uint16_t red_primary[2];
  uint16_t green_primary[2];
  uint16_t blue_primary[2];
  uint16_t white_point[2];
  uint32_t max_mastering_luminance;
  uint32_t min_mastering_luminance;
  uint16_t max_content_light_level;
  uint16_t max_frame_average_light_level;
};

WINEMETAL_API bool
WMTQueryDisplaySetting(uint32_t display_id, enum WMTColorSpace *colorspace, struct WMTHDRMetadata *metadata);

WINEMETAL_API void
WMTUpdateDisplaySetting(uint32_t display_id, enum WMTColorSpace colorspace, const struct WMTHDRMetadata *metadata);

WINEMETAL_API void WMTQueryDisplaySettingForLayer(
    obj_handle_t layer, uint64_t *version, enum WMTColorSpace *colorspace, struct WMTHDRMetadata *metadata,
    struct WMTEDRValue *edr_value
);

WINEMETAL_API void MTLCommandBuffer_encodeWaitForEvent(obj_handle_t cmdbuf, obj_handle_t event, uint64_t value);

WINEMETAL_API void MTLSharedEvent_signalValue(obj_handle_t event, uint64_t value);

WINEMETAL_API void MTLSharedEvent_setWin32EventAtValue(
    obj_handle_t event, obj_handle_t shared_event_listener, void *nt_event_handle, uint64_t at_value
);

WINEMETAL_API obj_handle_t MTLDevice_newFence(obj_handle_t device);

WINEMETAL_API obj_handle_t MTLDevice_newEvent(obj_handle_t device);

WINEMETAL_API void
MTLBuffer_updateContents(obj_handle_t buffer, uint64_t offset, struct WMTConstMemoryPointer data, uint64_t length);

WINEMETAL_API obj_handle_t SharedEventListener_create();

WINEMETAL_API void SharedEventListener_start(obj_handle_t shared_event_listener);

WINEMETAL_API void SharedEventListener_destroy(obj_handle_t shared_event_listener);

WINEMETAL_API void WMTGetOSVersion(uint64_t *major, uint64_t *minor, uint64_t *patch);

enum WMTMetalVersion : uint32_t {
  WMTMetal310 = 310,
  WMTMetal320 = 320,
  WMTMetalVersionMax = WMTMetal320,
};

WINEMETAL_API obj_handle_t MTLDevice_newBinaryArchive(obj_handle_t device, const char *url, obj_handle_t *err_out);

WINEMETAL_API void MTLBinaryArchive_serialize(obj_handle_t archive, const char *url, obj_handle_t *err_out);

WINEMETAL_API obj_handle_t DispatchData_alloc_init(uint64_t native_ptr, uint64_t length);

WINEMETAL_API obj_handle_t CacheReader_alloc_init(const char *path, uint64_t version);

WINEMETAL_API obj_handle_t CacheReader_get(obj_handle_t reader, const void *key, uint64_t length);

WINEMETAL_API obj_handle_t CacheWriter_alloc_init(const char *path, uint64_t version);

WINEMETAL_API bool CacheWriter_set(obj_handle_t writer, const void *key, uint64_t key_length, obj_handle_t value);

// gw2 (W2/W3): rows of a cache database (path and version as CacheReader_alloc_init's) whose key
// starts with `prefix` (256: any), after `after_rowid`, packed into `buffer` as u32 key length, key,
// u32 value length, value (little-endian). Returns the rows written; `last_rowid` is the next call's
// after_rowid; `more` says rows remain. 0 rows and `more` false when the database or table is missing.
// gw2 (#114): `status` 0: read (to the end, or the buffer is full); 1: no database or table; 2: a step
// failed, SQLite's code in `error` (SQLITE_BUSY after `busy` retries), the rows before it are valid.
// gw2 (#117): status 1 only for a missing file or a schema without the table (a new or empty file); a
// file that won't open or whose statement won't prepare (SQLITE_NOTADB, SQLITE_CORRUPT, …) is status 2.
// With `more`, `need` is the size of the row that didn't fit and `need_rowid` its rowid: with 0 rows
// it alone is larger than `capacity` (grow the buffer, or pass `need_rowid` as after_rowid to skip it).
struct WMTCacheEnumerateResult {
  uint64_t used;
  uint64_t last_rowid;
  uint64_t need;
  uint64_t need_rowid;
  uint32_t count;
  uint32_t status;
  uint32_t error;
  uint32_t busy;
  bool more;
};
WINEMETAL_API uint32_t CacheEnumerate(const char *path, uint64_t version, uint32_t prefix, uint64_t after_rowid,
                                      void *buffer, uint64_t capacity, struct WMTCacheEnumerateResult *result);

// gw2 (#57): the database file's size in bytes (page_count * page_size); 0 if unknown
WINEMETAL_API uint64_t CacheWriter_byteSize(obj_handle_t writer);

WINEMETAL_API bool WMTSetMetalShaderCachePath(const char *path);

/* gw2 (#46): the shader cache path Metal actually uses (MTLGetShaderCachePath, private API), as
   UTF-8 into buf (truncated to size - 1, always NUL-terminated); returns the full length, 0 if
   unknown. Metal fixes the path at its first device creation, which in Wine happens before
   dxgi.dll loads (winemac's GPU enumeration), so WMTSetMetalShaderCachePath can't change it. */
WINEMETAL_API uint32_t WMTGetMetalShaderCachePath(char *buf, uint32_t size);

WINEMETAL_API obj_handle_t MTLDevice_newSharedTexture(obj_handle_t device, struct WMTTextureInfo *info);

WINEMETAL_API bool WMTBootstrapRegister(const char *name, mach_port_t mach_port);

WINEMETAL_API bool WMTBootstrapLookUp(const char *name, mach_port_t *mach_port);

WINEMETAL_API mach_port_t MTLSharedEvent_createMachPort(obj_handle_t event);

WINEMETAL_API obj_handle_t MTLDevice_newSharedEventWithMachPort(obj_handle_t device, mach_port_t mach_port);

WINEMETAL_API uint64_t MTLDevice_registryID(obj_handle_t device);

WINEMETAL_API bool MTLSharedEvent_waitUntilSignaledValue(obj_handle_t event, uint64_t value, uint64_t timeout);

WINEMETAL_API obj_handle_t
MTLCounterSampleBuffer_newTimestampBuffer(obj_handle_t device, uint32_t sample_count, bool shared);

WINEMETAL_API void MTLCounterSampleBuffer_resolveCounterRange(
    obj_handle_t sample_buffer, uint32_t start, uint32_t len, void *data_out, uint64_t data_length
);

struct WMTSampleBufferAttachmentInfo {
  obj_handle_t sample_buffer;
  uint64_t start_of_encoder_sample_index;
  uint64_t end_of_encoder_sample_index;
};

WINEMETAL_API obj_handle_t MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(
    obj_handle_t cmdbuf, struct WMTSampleBufferAttachmentInfo *sample_buffer_attachments,
    uint64_t num_sample_buffer_attachments
);

enum WMTCommandBufferProperty : uint32_t {
  WMTCommandBufferPropertyKernelStartTime,
  WMTCommandBufferPropertyKernelEndTime,
  WMTCommandBufferPropertyGPUStartTime,
  WMTCommandBufferPropertyGPUEndTime,
};

WINEMETAL_API uint64_t MTLCommandBuffer_property(obj_handle_t cmdbuf, enum WMTCommandBufferProperty prop);

struct WMTTileRenderPipelineInfo {
  enum WMTPixelFormat color_formats[8];
  obj_handle_t tile_function;
  uint32_t immutable_tile_buffers;
  uint8_t raster_sample_count;
  bool tgsize_matches_tile_size;
  obj_handle_t binary_archive_for_serialization;
  struct WMTConstMemoryPointer binary_archives_for_lookup;
  uint8_t num_binary_archives_for_lookup;
  bool fail_on_binary_archive_miss;
  uint8_t padding[6];
};

WINEMETAL_API obj_handle_t MTLDevice_newTileRenderPipelineState(
    obj_handle_t device, const struct WMTTileRenderPipelineInfo *info, obj_handle_t *err_out
);

WINEMETAL_API obj_handle_t
MTLDevice_newResidencySet(obj_handle_t device, uint64_t init_capacity, obj_handle_t *err_out);

WINEMETAL_API void
MTLResidencySet_addAllocations(obj_handle_t residency_set, const obj_handle_t *allocations, uint64_t count);

WINEMETAL_API void
MTLResidencySet_removeAllocations(obj_handle_t residency_set, const obj_handle_t *allocations, uint64_t count);

WINEMETAL_API void MTLResidencySet_removeAllAllocations(obj_handle_t residency_set);

WINEMETAL_API void MTLResidencySet_commit(obj_handle_t residency_set);

WINEMETAL_API void MTLCommandQueue_addResidencySet(obj_handle_t queue, obj_handle_t residency_set);

enum WMTHeapType : uint32_t {
  WMTHeapTypeAutomatic = 0,
  WMTHeapTypePlacement = 1,
  WMTHeapTypeSparse = 2,
};
enum WMTSparsePageSize : uint32_t {
  WMTSparsePageSize16 = 101,
  WMTSparsePageSize64 = 102,
  WMTSparsePageSize256 = 103,
};
struct WMTHeapInfo {
  uint64_t size;
  enum WMTResourceOptions options;
  enum WMTHeapType type;
  enum WMTSparsePageSize sparse_page_size;
};

STATIC_ASSERT(sizeof(WMTHeapInfo) == 24);

WINEMETAL_API obj_handle_t MTLDevice_newHeap(obj_handle_t device, const struct WMTHeapInfo *info);

struct WMTSizeAndAlign {
  // 32-bit is sufficient
  uint32_t size;
  uint32_t align;
};

WINEMETAL_API struct WMTSizeAndAlign
MTLDevice_heapBufferSizeAndAlign(obj_handle_t device, uint64_t length, enum WMTResourceOptions options);

WINEMETAL_API struct WMTSizeAndAlign
MTLDevice_heapTextureSizeAndAlign(obj_handle_t device, struct WMTTextureInfo *info);

WINEMETAL_API obj_handle_t MTLHeap_newBuffer(obj_handle_t heap, struct WMTBufferInfo *info, uint64_t offset);

WINEMETAL_API obj_handle_t MTLHeap_newTexture(obj_handle_t heap, struct WMTTextureInfo *info, uint64_t offset);

enum WMTIndirectCommandType {
  WMTIndirectCommandTypeDraw = (1 << 0),
  WMTIndirectCommandTypeDrawIndexed = (1 << 1),
  WMTIndirectCommandTypeDrawPatches = (1 << 2),
  WMTIndirectCommandTypeDrawIndexedPatches = (1 << 3),
  WMTIndirectCommandTypeConcurrentDispatch = (1 << 5),
  WMTIndirectCommandTypeConcurrentDispatchThreads = (1 << 6),
  WMTIndirectCommandTypeDrawMeshThreadgroups = (1 << 7),
  WMTIndirectCommandTypeDrawMeshThreads = (1 << 8),
};

struct WMTIndirectCommandBufferInfo {
  enum WMTIndirectCommandType type;
  uint32_t inherit_buffers                  : 1;
  uint32_t inherit_pso                      : 1;
  uint32_t inherit_cull_mode                : 1;
  uint32_t inherit_depth_bias               : 1;
  uint32_t inherit_depth_clip_mode          : 1;
  uint32_t inherit_depth_stencil_state      : 1;
  uint32_t inherit_front_facing             : 1;
  uint32_t inherit_fill_mode                : 1;
  uint32_t support_ray_tracing              : 1;
  uint32_t support_dynamic_attribute_stride : 1;
  uint32_t support_color_attachment_mapping : 1;
  uint32_t padding_bits                     : 21;
  uint8_t max_vertex_buffer_binding;
  uint8_t max_fragment_buffer_binding;
  uint8_t max_kernel_buffer_binding;
  uint8_t max_mesh_buffer_binding;
  uint8_t max_object_buffer_binding;
  uint8_t max_object_threadgroup_memory_binding;
  uint8_t max_kernel_threadgroup_memory_binding;
  uint8_t padding;
  uint64_t gpu_resource_id; // out
};

WINEMETAL_API obj_handle_t MTLDevice_newIndirectCommandBuffer(
    obj_handle_t device, struct WMTIndirectCommandBufferInfo *info, uint64_t max_count, enum WMTResourceOptions options
);

WINEMETAL_API obj_handle_t
MTLDevice_newLibraryWithSource(obj_handle_t device, const char *source, uint64_t length, obj_handle_t *err_out);

WINEMETAL_API void MTLTexture_getBytes(
    obj_handle_t texture, struct WMTOrigin origin, struct WMTSize size, uint64_t level, uint64_t slice,
    struct WMTMemoryPointer data, uint64_t bytes_per_row, uint64_t bytes_per_image
);

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

/* gw2 (#5): persistent Metal binary archives for pipelines (DXMT_PSO_ARCHIVE=1). The unix side
   owns the archives: it loads valid ones of earlier sessions for lookup, adds every render and
   compute pipeline it builds to this session's archive (on a background queue), and serializes
   that archive on WMTPSOArchiveFlush and at exit. Reports are UTF-8 lines separated by '\n'. */
struct WMTPSOArchiveConfig {
  struct WMTConstMemoryPointer exe_name; // UTF-8; the unix side sanitizes it
  struct WMTConstMemoryPointer base_dir; // absolute unix directory, or null for the user cache dir
  uint32_t keep_sessions;
  uint32_t max_pipelines; // per session archive
  // gw2 (#75): DXMT_PSO_GENERATIONS=1: the generation publisher (immutable shards, checksummed
  // manifests, an atomically replaced index) and the exact-shape failure policy; 0: one file per
  // session, as #52 left it
  uint32_t generations;
  uint32_t padding;
};

struct WMTPSOArchiveStats {
  uint64_t loaded;        // archives of earlier sessions in use for lookup
  uint64_t lookup_hits;   // pipelines found in those archives
  uint64_t lookup_misses; // pipelines built without a hit
  uint64_t added;         // pipelines added to this session's archive
  uint64_t add_failed;
  uint64_t capped;        // not added: max_pipelines reached
  uint64_t queued;        // adds still waiting on the archive queue
  uint64_t unflushed;     // added since the last serialization
  uint64_t flushes;
  uint64_t flushed_bytes; // size of the last serialized archive
  // gw2 (#52), since start:
  uint64_t miss_known;    // lookup misses whose key an earlier session's .keys holds
  uint64_t miss_new;      // lookup misses never archived before
  uint64_t skipped;       // pipelines left out after Metal's packer named them
  uint64_t rebuilds;      // flushes that succeeded only after a rebuild
  // gw2 (#75), with generations only (0 otherwise):
  uint64_t mode;          // 0: session files (or off), 1: generations, 2: generations, lookup only
  uint64_t miss_excluded; // lookup misses of a failing shape an earlier generation excluded
  uint64_t excluded;      // adds refused as a failing shape (exact-shape policy)
  uint64_t generation;    // the generation loaded, then the last one published
  uint64_t published;     // pipelines in this session's shard of the last published generation
};

WINEMETAL_API bool WMTPSOArchiveInit(
    obj_handle_t device, const struct WMTPSOArchiveConfig *config, char *report, uint32_t report_size
);

/* Serializes this session's archive if anything was added since the last time; blocks the caller
   until earlier adds are done. Returns 1 when written, 0 when nothing was new, -1 on failure. */
WINEMETAL_API int WMTPSOArchiveFlush(char *report, uint32_t report_size);

WINEMETAL_API void WMTPSOArchiveQuery(struct WMTPSOArchiveStats *stats);

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
     work retired. 0: no set; UseResource then counts in use_resource_dropped. */
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
#define WMT_MTL4_NO_ATTACHMENT_RESIDENCY 0x1u

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