// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O2 -Wall -Wextra -Isrc/dxmt -Isrc/winemetal tests/gw2/texture_view_key_test.cpp -o $OUT
//
// gw2 (#155): the texture view key's array bit (src/dxmt/dxmt_texture_view_key.hpp). No Metal, no Wine.
//
// - For each of Metal's texture types, a key built from a view of that type has the array bit
//   ARRAY_TYPE_MASK gives it (the mask copied from Texture::checkViewUseArray at 79c1bb3), and the
//   other fields as before. The bit survives the key's round trip through uint64_t.
// - The index keeps 27 bits: the largest index the limit allows doesn't touch the bit or the mip
//   count.
// - resolveViewUseArray, the body of Texture::checkViewUseArray, against a fake texture (its
//   descriptor list and a createView with Texture::createView's search): for each type, a shader
//   that wants an array and one that doesn't, with the bit trusted and with DXMT_VIEW_KEY_ARRAY_BIT=0.
//   A match returns the key; with the bit trusted it reads no descriptor (no lock). A mismatch
//   returns the counterpart view's key (its type and slice count from a table here, its bit set),
//   created once and found again by a second mismatch; 3D and texture buffers keep their key.

#include "dxmt_texture_view_key.hpp"

#include <cstdio>
#include <vector>

using namespace dxmt;

static int failures = 0;
static int checks = 0;
#define CHECK(...)                                                                                 \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(__VA_ARGS__)) {                                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// Texture::checkViewUseArray at 79c1bb3: `bool((1 << uint32_t(view.type)) & ARRAY_TYPE_MASK)`
static constexpr uint32_t ARRAY_TYPE_MASK = 0b0101001010;

struct TypeCase {
  WMTTextureType type;
  const char *name;
  bool array;
  bool has_counterpart;
  WMTTextureType counterpart;
  uint32_t counterpart_slices;
};

static const TypeCase kTypes[] = {
    {WMTTextureType1D, "1D", false, true, WMTTextureType1DArray, 1},
    {WMTTextureType1DArray, "1DArray", true, true, WMTTextureType1D, 1},
    {WMTTextureType2D, "2D", false, true, WMTTextureType2DArray, 1},
    {WMTTextureType2DArray, "2DArray", true, true, WMTTextureType2D, 1},
    {WMTTextureType2DMultisample, "2DMultisample", false, true, WMTTextureType2DMultisampleArray, 1},
    {WMTTextureTypeCube, "Cube", false, true, WMTTextureTypeCubeArray, 6},
    {WMTTextureTypeCubeArray, "CubeArray", true, true, WMTTextureTypeCube, 6},
    {WMTTextureType3D, "3D", false, false, WMTTextureType3D, 0},
    {WMTTextureType2DMultisampleArray, "2DMultisampleArray", true, true, WMTTextureType2DMultisample, 1},
    {WMTTextureTypeTextureBuffer, "TextureBuffer", false, false, WMTTextureTypeTextureBuffer, 0},
};

static TextureViewDescriptor
descriptorOf(WMTTextureType type) {
  return {
      .format = WMTPixelFormatRGBA8Unorm,
      .type = type,
      .firstMiplevel = 1,
      .miplevelCount = 3,
      .firstArraySlice = type == WMTTextureTypeCube || type == WMTTextureTypeCubeArray ? 6u : 2u,
      .arraySize = type == WMTTextureTypeCube || type == WMTTextureTypeCubeArray ? 12u : 4u,
  };
}

static bool
sameDescriptor(const TextureViewDescriptor &a, const TextureViewDescriptor &b) {
  return a.format == b.format && a.type == b.type && a.firstMiplevel == b.firstMiplevel &&
         a.miplevelCount == b.miplevelCount && a.firstArraySlice == b.firstArraySlice && a.arraySize == b.arraySize;
}

// A texture's view list, as Texture keeps it: createView finds an equal descriptor or appends one.
struct FakeTexture {
  static constexpr unsigned kMips = 5;
  std::vector<TextureViewDescriptor> views;
  int describes = 0;
  int creates = 0;

  TextureViewKey
  createView(const TextureViewDescriptor &descriptor) {
    unsigned i = 0;
    for (; i < views.size(); i++)
      if (sameDescriptor(views[i], descriptor))
        return TextureViewKey(descriptor, i, kMips);
    views.push_back(descriptor);
    return TextureViewKey(descriptor, i, kMips);
  }

  TextureViewKey
  check(TextureViewKey key, bool isArray, bool trustBit) {
    return resolveViewUseArray(
        key, isArray, trustBit,
        [this](unsigned index) {
          describes++;
          return views[index];
        },
        [this](const TextureViewDescriptor &descriptor) {
          creates++;
          return createView(descriptor);
        }
    );
  }
};

static void
keyBit() {
  for (auto &c : kTypes) {
    bool mask = (1u << uint32_t(c.type)) & ARRAY_TYPE_MASK;
    CHECK(mask == c.array); // the table here agrees with the old mask
    CHECK(isArrayTextureType(c.type) == mask);

    auto d = descriptorOf(c.type);
    TextureViewKey key(d, 7, 9);
    CHECK(bool(key.array) == mask);
    CHECK(key.index == 7);
    CHECK(key.mip_count == 9);
    CHECK(key.mip_start == 1);
    CHECK(key.mip_end == 4);
    CHECK(key.array_start == d.firstArraySlice);
    CHECK(key.array_end == d.firstArraySlice + d.arraySize);

    TextureViewKey round{uint64_t(key)};
    CHECK(round.impl_ == key.impl_);
    CHECK(bool(round.array) == mask);
    CHECK(round.index == 7);
  }
  TextureViewKey empty;
  CHECK(empty.impl_ == 0 && empty.array == 0);
}

static void
indexRoom() {
  static_assert(TextureViewKey::kIndexBits == 27);
  static_assert(TextureViewKey::kIndexLimit == 134217728);
  unsigned last = unsigned(TextureViewKey::kIndexLimit - 1);
  for (bool array : {false, true}) {
    auto d = descriptorOf(array ? WMTTextureType2DArray : WMTTextureType2D);
    TextureViewKey key(d, last, 15);
    CHECK(key.index == last);
    CHECK(bool(key.array) == array);
    CHECK(key.mip_count == 15);
    CHECK(key.mip_start == 1);
    CHECK((key.impl_ & (TextureViewKey::kIndexLimit - 1)) == last);
    CHECK(bool(key.impl_ & TextureViewKey::kIndexLimit) == array);
  }
}

static void
resolve() {
  for (auto &c : kTypes) {
    for (bool trust : {true, false}) {
      for (bool wants : {false, true}) {
        FakeTexture t;
        // the view under test at a nonzero index, after the texture's full view
        t.createView(descriptorOf(WMTTextureType2D));
        auto d = descriptorOf(c.type);
        auto key = t.createView(d);
        CHECK(key.index == (c.type == WMTTextureType2D ? 0u : 1u));
        size_t views_before = t.views.size();

        auto got = t.check(key, wants, trust);
        if (wants == c.array) {
          CHECK(got.impl_ == key.impl_);
          CHECK(t.describes == (trust ? 0 : 1));
          CHECK(t.creates == 0);
          continue;
        }
        CHECK(t.describes == 1);
        if (!c.has_counterpart) {
          CHECK(got.impl_ == key.impl_);
          CHECK(t.creates == 0);
          CHECK(t.views.size() == views_before);
          continue;
        }
        CHECK(t.creates == 1);
        CHECK(t.views.size() == views_before + 1);
        CHECK(got.index == views_before);
        auto &converted = t.views[got.index];
        CHECK(converted.type == c.counterpart);
        CHECK(converted.arraySize == c.counterpart_slices);
        CHECK(converted.format == d.format);
        CHECK(converted.firstMiplevel == d.firstMiplevel && converted.miplevelCount == d.miplevelCount);
        CHECK(converted.firstArraySlice == d.firstArraySlice);
        CHECK(bool(got.array) == wants);
        CHECK(got.array_end - got.array_start == c.counterpart_slices);

        // the converted key now matches: with the bit trusted, no descriptor is read
        int describes = t.describes;
        auto again = t.check(got, wants, trust);
        CHECK(again.impl_ == got.impl_);
        CHECK(t.describes == describes + (trust ? 0 : 1));

        // a second mismatch through the original key finds the same view
        auto second = t.check(key, wants, trust);
        CHECK(second.impl_ == got.impl_);
        CHECK(t.views.size() == views_before + 1);
      }
    }
  }
}

int
main() {
  keyBit();
  indexRoom();
  resolve();
  std::printf("texture_view_key: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
