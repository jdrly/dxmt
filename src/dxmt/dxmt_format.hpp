#pragma once

#include "Metal.hpp"
#include <map>

namespace dxmt {
enum class FormatCapability : int {
  None = 0,
  Atomic = 0x1,
  Filter = 0x2,
  Write = 0x4,
  Color = 0x8,
  Blend = 0x10,
  MSAA = 0x20,
  Sparse = 0x40,
  Resolve = 0x80,
  DepthStencil = 0x100,
  TextureBufferRead = 0x200,
  TextureBufferWrite = 0x400,
  TextureBufferReadWrite = 0x800
};

class FormatCapabilityInspector {
public:
  std::map<WMTPixelFormat, FormatCapability> textureCapabilities{};
  void Inspect(WMT::Device device);
};

WMTPixelFormat Forget_sRGB(WMTPixelFormat format);
WMTPixelFormat Recall_sRGB(WMTPixelFormat format);

inline bool
Is_sRGBVariant(WMTPixelFormat format) {
  return Forget_sRGB(format) != format;
}

bool IsBlockCompressionFormat(WMTPixelFormat format);

uint32_t DepthStencilPlanarFlags(WMTPixelFormat format);

enum MTL_DXGI_FORMAT_FLAG {
  MTL_DXGI_FORMAT_TYPELESS = 1,
  MTL_DXGI_FORMAT_BC = 2,
  MTL_DXGI_FORMAT_BACKBUFFER = 4,
  MTL_DXGI_FORMAT_DEPTH_PLANER = 16,
  MTL_DXGI_FORMAT_STENCIL_PLANER = 32,
  MTL_DXGI_FORMAT_EMULATED_D24 = 256,
  MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL = 512,
};

struct MTL_DXGI_FORMAT_DESC {
  WMTPixelFormat PixelFormat;
  WMTAttributeFormat AttributeFormat;
  union {
    uint32_t BytesPerTexel;
    uint32_t BlockSize;
  };
  uint16_t PlanarCount;
  uint16_t Flag;
};

int32_t MTLQueryDXGIFormat(WMT::Device device, uint32_t format, MTL_DXGI_FORMAT_DESC &description);

uint32_t MTLGetTexelSize(WMTPixelFormat format);

// gw2 (#202): a depth or stencil format's bytes per texel, which MTLGetTexelSize leaves at 0 (its
// callers copy and initialize color data). For the CREATE line's texture bytes only; 0 for any other
// format. Depth32Float_Stencil8 (D24S8 and D32S8 here) takes 8 bytes, a 32-bit depth and 8-bit stencil
// padded.
inline uint32_t
gw2DepthTexelSize(WMTPixelFormat format) {
  switch (ORIGINAL_FORMAT(format)) {
  case WMTPixelFormatStencil8:
    return 1;
  case WMTPixelFormatDepth16Unorm:
    return 2;
  case WMTPixelFormatDepth32Float:
  case WMTPixelFormatDepth24Unorm_Stencil8:
  case WMTPixelFormatX24_Stencil8:
    return 4;
  case WMTPixelFormatDepth32Float_Stencil8:
  case WMTPixelFormatX32_Stencil8:
    return 8;
  default:
    return 0;
  }
}

WMTPixelFormat MTLGetUnsignedIntegerFormat(WMTPixelFormat format);

bool IsUnorm8RenderTargetFormat(WMTPixelFormat format);

bool IsIntegerFormat(WMTPixelFormat format);

void SanitizeRTVClearColor(WMTPixelFormat format, WMTClearColor &color);

} // namespace dxmt