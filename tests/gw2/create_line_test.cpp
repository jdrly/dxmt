// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -DGW2_TEST_REAL_ENV -Itests/gw2/shim -Isrc/dxmt -Isrc/util -Isrc/winemetal tests/gw2/create_line_test.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp -o $OUT
// gw2-run: env -u DXMT_STATS "$OUT" on
// gw2-run: DXMT_STATS=0 "$OUT" off
//
// gw2 (#202): host-side test of the CREATE line's counting (src/dxmt/dxmt_gw2_stats.hpp): CreateScope,
// CreateTotals and createLine. Simulated Win32 threads (shim/windows.h), DXMT_STATS read from the
// environment (GW2_TEST_REAL_ENV), so it runs twice:
//
// - on: the size classes at their edges, the buffer usage split, the render and depth targets, the
//   texture bytes estimate (mip chains, BC blocks, 3D, layers), and the depth formats' texel bytes
//   (gw2DepthTexelSize, src/dxmt/dxmt_format.hpp) on the Metal formats MTLQueryDXGIFormat gives D3D11's
//   depth formats (dxmt_format.cpp needs dxgi.h, so the mapping itself isn't built here). The kinds' calls and time equal PACE's
//   `create` on both paths, the presenting thread's interval and the other threads' pool; a nested
//   Create* counts once, under the outer call's kind and description; a Create* inside another entry
//   point counts in neither. A create still reads the clock twice. The line's fields and its absence
//   in a window without a create.
// - off (DXMT_STATS=0): a create reads no clock, isn't counting, and leaves every counter at 0.
//
// Run: tests/gw2/run.zsh pure, or the commands above with $OUT set.

#include "dxmt_format.hpp"
#include "dxmt_gw2_stats.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace dxmt::gw2;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

constexpr unsigned kCreate = (unsigned)Api::Create;
constexpr unsigned kBuf = (unsigned)CreateKind::Buffer, kTex = (unsigned)CreateKind::Texture;

// A device entry point as d3d11_device.cpp writes it: the scope, then the description if counted.
static void
createBuffer(uint64_t bytes, uint32_t usage, bool init) {
  GW2_CREATE_SCOPE(Buffer);
  if (gw2_create_scope_.counting())
    gw2_create_scope_.resource(bytes, usage, 0, init);
}

// CreateTexture2D -> CreateTexture2D1: both describe, only the outer one counts
static void
createTexture2D1(uint64_t bytes, uint32_t bind) {
  GW2_CREATE_SCOPE(Texture);
  if (gw2_create_scope_.counting())
    gw2_create_scope_.resource(bytes, 0, bind, false);
}
static void
createTexture2D(uint64_t bytes, uint32_t bind) {
  GW2_CREATE_SCOPE(Texture);
  if (gw2_create_scope_.counting())
    gw2_create_scope_.resource(bytes, 0, bind, true);
  createTexture2D1(bytes * 1000, 0); // a different description: must not be the one counted
}

static void
createView() {
  GW2_CREATE_SCOPE(View);
}

// A mix of every path: 4 buffers, 2 textures (each a nested pair), 1 view; 7 counted calls.
static void
mix() {
  createBuffer(100, 0, true);
  createBuffer(64 << 10, 2, false);
  createBuffer(2 << 20, 2, false);
  createBuffer(32 << 20, 3, false);
  createTexture2D(4 << 20, kBindRenderTarget);
  createTexture2D(1 << 10, kBindDepthStencil);
  createView();
  {
    ApiScope flush(Api::Flush, false); // a create inside another entry point counts in neither
    createBuffer(1, 0, false);
  }
}

static void
checkMix(const CreateTotals &t) {
  CHECK(t.totalCalls() == 7);
  CHECK(t.calls[kBuf] == 4 && t.calls[kTex] == 2 && t.calls[(unsigned)CreateKind::View] == 1);
  CHECK(t.bytes[kBuf] == 100 + (64u << 10) + (2u << 20) + (32u << 20));
  CHECK(t.bytes[kTex] == (4u << 20) + (1u << 10)); // the outer descriptions
  CHECK(t.init[kBuf] == 1 && t.init[kTex] == 2);
  CHECK(t.size[kBuf][0] == 1 && t.size[kBuf][1] == 1 && t.size[kBuf][2] == 1 && t.size[kBuf][3] == 1);
  CHECK(t.size[kTex][0] == 1 && t.size[kTex][2] == 1);
  CHECK(t.usage[0] == 1 && t.usage[1] == 0 && t.usage[2] == 2 && t.usage[3] == 1);
  CHECK(t.rt == 1 && t.ds == 1);
}

static int64_t
sumNs(const CreateTotals &t) {
  int64_t ns = 0;
  for (auto v : t.ns)
    ns += v;
  return ns;
}

static void
pure() {
  // size classes at their edges
  CHECK(sizeClass(0) == 0 && sizeClass((64 << 10) - 1) == 0);
  CHECK(sizeClass(64 << 10) == 1 && sizeClass((1 << 20) - 1) == 1);
  CHECK(sizeClass(1 << 20) == 2 && sizeClass(16 << 20) == 2);
  CHECK(sizeClass((16 << 20) + 1) == 3 && sizeClass(uint64_t(1) << 40) == 3);

  // texture bytes
  CHECK(textureBytes(256, 256, 1, 1, 1, 4, false) == 256 * 256 * 4);
  CHECK(textureBytes(256, 256, 1, 1, 0, 4, false) == 4 * 87381); // 9 levels down to 1x1
  CHECK(textureBytes(8, 2, 1, 1, 0, 1, false) == 16 + 4 + 2 + 1); // the chain follows the longer side
  CHECK(textureBytes(256, 256, 1, 1, 1, 8, true) == 64 * 64 * 8); // BC1: 8 bytes a 4x4 block
  CHECK(textureBytes(2, 2, 1, 1, 1, 16, true) == 16);            // a partial block is a block
  CHECK(textureBytes(16, 16, 16, 1, 0, 4, false) == 4 * (4096 + 512 + 64 + 8 + 1));
  CHECK(textureBytes(64, 64, 1, 6, 1, 4, false) == 6 * 64 * 64 * 4);
  CHECK(textureBytes(64, 64, 1, 0, 1, 4, false) == 64 * 64 * 4); // 0 layers: one
  CHECK(textureBytes(64, 64, 1, 1, 1, 0, false) == 0);           // unknown format
  CHECK(textureBytes(1, 1, 1, 1, 100, 4, false) == 32 * 4);      // a bad mip count is capped

  // depth formats: MTLGetTexelSize has none, so CREATE takes gw2DepthTexelSize. The Metal formats are
  // MTLQueryDXGIFormat's for the DXGI formats named.
  using dxmt::gw2DepthTexelSize;
  CHECK(gw2DepthTexelSize(WMTPixelFormatDepth32Float_Stencil8) == 8); // R24G8_TYPELESS, D24_UNORM_S8_UINT,
                                                                       // R32G8X24_TYPELESS, D32_FLOAT_S8X24_UINT
  CHECK(gw2DepthTexelSize(WMTPixelFormatR32X8X32) == 8);    // R24_UNORM_X8_TYPELESS, R32_FLOAT_X8X24_TYPELESS
  CHECK(gw2DepthTexelSize(WMTPixelFormatX32G8X32) == 8);    // X24_TYPELESS_G8_UINT, X32_TYPELESS_G8X24_UINT
  CHECK(gw2DepthTexelSize(WMTPixelFormatDepth32Float) == 4); // D32_FLOAT
  CHECK(gw2DepthTexelSize(WMTPixelFormatDepth16Unorm) == 2); // D16_UNORM
  CHECK(gw2DepthTexelSize(WMTPixelFormatDepth24Unorm_Stencil8) == 4);
  CHECK(gw2DepthTexelSize(WMTPixelFormatStencil8) == 1);
  CHECK(gw2DepthTexelSize(WMTPixelFormatRGBA8Unorm) == 0); // color formats: MTLGetTexelSize's
  // GW2's 4K depth target (R24G8_TYPELESS): 66 MB, over 16 MB, not 0 bytes under 64 KB
  uint64_t depth4k = textureBytes(3840, 2160, 1, 1, 1, gw2DepthTexelSize(WMTPixelFormatDepth32Float_Stencil8), false);
  CHECK(depth4k == 3840ull * 2160 * 8 && sizeClass(depth4k) == 3);

  // the usage split and the targets, from calls
  CreateTotals t;
  for (uint32_t usage = 0; usage < 4; usage++)
    for (uint32_t n = 0; n <= usage; n++)
      t.add({CreateKind::Buffer, 10, usage, 0, false}, 1);
  t.add({CreateKind::Buffer, 10, 7, 0, false}, 1); // no such usage: a call, no usage bucket
  t.add({CreateKind::Texture, 10, 0, kBindRenderTarget | kBindDepthStencil, false}, 1);
  t.add({CreateKind::Texture, 10, 0, kBindDepthStencil, false}, 1);
  t.add({CreateKind::Texture, 10, 2, 0, false}, 1); // a texture's usage isn't counted
  t.add({CreateKind::Shader, 1 << 30, 0, kBindRenderTarget, true}, 1); // only buffers and textures
  CHECK(t.usage[0] == 1 && t.usage[1] == 2 && t.usage[2] == 3 && t.usage[3] == 4);
  CHECK(t.calls[kBuf] == 11 && t.calls[kTex] == 3 && t.calls[(unsigned)CreateKind::Shader] == 1);
  CHECK(t.rt == 1 && t.ds == 2);
  CHECK(t.init[kBuf] == 0 && t.init[kTex] == 0);
  CHECK(t.bytes[kBuf] == 110 && t.bytes[kTex] == 30);
  CHECK(t.size[kBuf][0] == 11 && t.size[kTex][0] == 3);
  CHECK(sumNs(t) == 15);
  CreateTotals twice = t;
  twice.add(t);
  CHECK(twice.totalCalls() == 2 * t.totalCalls() && twice.usage[3] == 8 && twice.rt == 2 && twice.ds == 4 &&
        twice.size[kBuf][0] == 22);

  // the line
  CHECK(createLine(CreateTotals{}, CreateTotals{}, 120).empty());
  CreateTotals rt;
  rt.add({CreateKind::Buffer, 2048, 2, 0, true}, 2'000'000);
  rt.add({CreateKind::View}, 1'000'000);
  std::string line = createLine(rt, CreateTotals{}, 2);
  std::printf("CREATE %s\n", line.c_str());
  CHECK(line.rfind("calls=1.00 ms=1.500 total=2 ", 0) == 0);
  CHECK(line.find(" buffer=0.50/1.000") != std::string::npos);
  CHECK(line.find(" view=0.50/0.500") != std::string::npos);
  CHECK(line.find(" buffer.kb=1.0 buffer.init=0.50 buffer.size=<64k:0.50/<1m:0.00/<=16m:0.00/>16m:0.00") !=
        std::string::npos);
  CHECK(line.find(" buffer.usage=default:0.00/immutable:0.00/dynamic:0.50/staging:0.00") != std::string::npos);
  CHECK(line.find("texture") == std::string::npos && line.find("other.") == std::string::npos);
  std::string other = createLine(CreateTotals{}, rt, 2);
  CHECK(other.rfind("calls=0.00 ms=0.000 total=0 other.calls=1.00 ", 0) == 0);
  CHECK(other.find(" other.buffer.usage=") != std::string::npos);
}

int
main(int argc, char **argv) {
  bool on = argc > 1 && std::strcmp(argv[1], "on") == 0;
  CHECK(statsEnabled() == on);

  if (!on) {
    fake::tid = 7;
    uint64_t reads0 = fake::qpc_reads;
    for (int i = 0; i < 100; i++) {
      GW2_CREATE_SCOPE(Texture);
      CHECK(!gw2_create_scope_.counting());
    }
    for (int i = 0; i < 100; i++)
      mix();
    CHECK(fake::qpc_reads == reads0);
    CHECK(otherDrain().calls[kCreate] == 0);
    CHECK(otherCreateDrain().totalCalls() == 0);
    CHECK(takeThreadInterval().create.totalCalls() == 0);
  } else {
    pure();
    clockFloorNs(); // measured once, on first use

    // another thread: the pool, drained by otherDrain and otherCreateDrain
    fake::tid = 7;
    otherDrain();
    otherCreateDrain();
    uint64_t reads0 = fake::qpc_reads;
    mix();
    CHECK(fake::qpc_reads - reads0 == 2 * 8); // 7 counted calls and the flush, 2 each; the nested buffer none
    ApiTotals pace = otherDrain();
    CreateTotals create = otherCreateDrain();
    checkMix(create);
    CHECK(pace.calls[kCreate] == create.totalCalls());
    CHECK(pace.ns[kCreate] == sumNs(create));

    // the presenting thread: its interval
    fake::tid = 9;
    takeThreadInterval();
    for (int i = 0; i < 3; i++)
      mix();
    ThreadInterval interval = takeThreadInterval();
    CreateTotals three = interval.create;
    CHECK(three.totalCalls() == 21 && three.rt == 3 && three.usage[2] == 6);
    CHECK(interval.api.calls[kCreate] == three.totalCalls());
    CHECK(interval.api.ns[kCreate] == sumNs(three));
    CHECK(otherCreateDrain().totalCalls() == 0); // nothing went to the pool
    CHECK(takeThreadInterval().create.totalCalls() == 0); // taken: starts over
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("create_line_test %s: ok\n", on ? "on" : "off");
  return 0;
}
