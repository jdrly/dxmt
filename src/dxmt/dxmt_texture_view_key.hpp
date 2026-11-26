#pragma once

// gw2 (#155): a texture's view descriptor and view key, and the check the encode thread makes for
// every texture a shader reads: does the bound view's type agree with the shader's wish for an array
// texture? The key carries the answer in one bit, so a match takes no lock. Pure C++ over
// winemetal.h: tests/gw2/texture_view_key_test.cpp builds it natively.

#include "winemetal.h"
#include <cstdint>

namespace dxmt {

struct TextureViewDescriptor {
  WMTPixelFormat format    : 24;
  WMTTextureType type      : 8;
  uint32_t firstMiplevel   : 4 = 0;
  uint32_t miplevelCount   : 4 = 1;
  uint32_t firstArraySlice : 12 = 0;
  uint32_t arraySize       : 12 = 1;
};

// Metal's array types: 1DArray, 2DArray, CubeArray and 2DMultisampleArray.
constexpr bool
isArrayTextureType(WMTTextureType type) {
  constexpr uint32_t ARRAY_TYPE_MASK = 0b0101001010;
  return (1u << uint32_t(type)) & ARRAY_TYPE_MASK;
}

struct TextureViewKey {
  // The view's index in its texture had 28 bits; since #155 one of them is `array`.
  static constexpr unsigned kIndexBits = 27;
  static constexpr uint64_t kIndexLimit = uint64_t(1) << kIndexBits;

  union {
    struct {
      uint64_t index       : kIndexBits;
      uint64_t array       : 1; // isArrayTextureType of the view's type
      uint64_t mip_count   : 4;
      uint64_t mip_start   : 4;
      uint64_t array_start : 12;
      uint64_t mip_end     : 4;
      uint64_t array_end   : 12;
    };
    uint64_t impl_;
  };

  TextureViewKey() {
    impl_ = 0;
  }
  // Every key of a view is built here (Texture::createView, Texture's constructors, TextureView), so
  // `array` always follows the type of the descriptor at `index`.
  TextureViewKey(const TextureViewDescriptor &descriptor, unsigned index, unsigned total_mip_count) {
    mip_start = descriptor.firstMiplevel;
    array_start = descriptor.firstArraySlice;
    mip_end = descriptor.firstMiplevel + descriptor.miplevelCount;
    array_end = descriptor.firstArraySlice + descriptor.arraySize;
    mip_count = total_mip_count;
    array = isArrayTextureType(descriptor.type);
    this->index = index;
  }
  TextureViewKey(uint64_t impl) {
    impl_ = impl;
  }
  operator uint64_t() const {
    return impl_;
  }
};
static_assert(sizeof(TextureViewKey) == sizeof(uint64_t));

// Turns `descriptor` into its array or non-array counterpart (a cube's counterpart keeps 6 slices,
// the others 1). false for a type without one (3D, texture buffer).
inline bool
toggleArrayType(TextureViewDescriptor &descriptor) {
  switch (descriptor.type) {
  case WMTTextureType1D:
    descriptor.type = WMTTextureType1DArray;
    descriptor.arraySize = 1;
    return true;
  case WMTTextureType1DArray:
    descriptor.type = WMTTextureType1D;
    descriptor.arraySize = 1;
    return true;
  case WMTTextureType2D:
    descriptor.type = WMTTextureType2DArray;
    descriptor.arraySize = 1;
    return true;
  case WMTTextureType2DArray:
    descriptor.type = WMTTextureType2D;
    descriptor.arraySize = 1;
    return true;
  case WMTTextureType2DMultisample:
    descriptor.type = WMTTextureType2DMultisampleArray;
    descriptor.arraySize = 1;
    return true;
  case WMTTextureType2DMultisampleArray:
    descriptor.type = WMTTextureType2DMultisample;
    descriptor.arraySize = 1;
    return true;
  case WMTTextureTypeCube:
    descriptor.type = WMTTextureTypeCubeArray;
    descriptor.arraySize = 6;
    return true;
  case WMTTextureTypeCubeArray:
    descriptor.type = WMTTextureTypeCube;
    descriptor.arraySize = 6;
    return true;
  default:
    return false;
  }
}

// The key of the view a shader that wants (`isArray`) or doesn't want an array texture reads through
// `key`: `key` itself when the view's type agrees, else the key `create(descriptor)` returns for the
// counterpart view (Texture::createView). `describe(index)` copies the descriptor of view `index`
// under the texture's lock. With `trustBit` a match is read from the key and describes nothing; a
// mismatch, and every check without it (DXMT_VIEW_KEY_ARRAY_BIT=0), goes by the descriptor, as before
// #155.
template <class Describe, class Create>
TextureViewKey
resolveViewUseArray(TextureViewKey key, bool isArray, bool trustBit, Describe &&describe, Create &&create) {
  if (trustBit && bool(key.array) == isArray)
    return key;
  TextureViewDescriptor view = describe(unsigned(key.index));
  if (isArrayTextureType(view.type) == isArray)
    return key;
  // TODO: this process can be cached
  if (!toggleArrayType(view))
    return key; // should be unreachable
  return create(view);
}

} // namespace dxmt
