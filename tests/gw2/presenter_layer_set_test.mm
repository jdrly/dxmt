// gw2-test: metal
// gw2-build: U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
// gw2-build: (( ${#U} >= 5 ))
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   for f in $U; do clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Wno-unused-function -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $T/u_${f:r}_$arch.o; done
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal src/winemetal/winemetal_thunks.c -o $T/thunks_$arch.o
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c -o $T/airconv_absent_$arch.o
// gw2-build:   clang++ -std=c++20 -fobjc-arc -Wall -arch $arch -mmacosx-version-min=26.0 -DDXMT_PAGE_SIZE=4096 -Itests/gw2/shim -Isrc/winemetal -Isrc/dxmt -Isrc/util -Iinclude tests/gw2/presenter_layer_set_test.mm src/dxmt/dxmt_presenter.cpp src/dxmt/dxmt_gw2_switches.cpp src/dxmt/dxmt_gw2_mtl4_queue.cpp src/dxmt/dxmt_gw2_mtl4_encoding.cpp $T/thunks_$arch.o $T/u_*_$arch.o $T/airconv_absent_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -o ${OUT}_$arch
// gw2-build:   clang++ -std=c++20 -fobjc-arc -Wall -arch $arch -mmacosx-version-min=26.0 -DDXMT_PAGE_SIZE=4096 -DDXMT_GW2_LIFETIME_CHECK=1 -DGW2_TEST_REAL_ENV=1 -Itests/gw2/shim -Isrc/winemetal -Isrc/dxmt -Isrc/util -Iinclude tests/gw2/presenter_layer_set_test.mm src/dxmt/dxmt_presenter.cpp src/dxmt/dxmt_gw2_mtl4_queue.cpp src/dxmt/dxmt_gw2_mtl4_encoding.cpp src/dxmt/dxmt_gw2_lifetime.cpp src/dxmt/dxmt_gw2_switches.cpp $T/thunks_$arch.o $T/u_*_$arch.o $T/airconv_absent_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -o ${OUT}_check_$arch
// gw2-build: done
// gw2-run: for arch in x86_64 arm64; do ${OUT}_$arch; MTL_DEBUG_LAYER=1 ${OUT}_$arch; MTL_SHADER_VALIDATION=1 ${OUT}_$arch; done
// gw2-run: for arch in x86_64 arm64; do for free in 0 1; do DXMT_LIFETIME_CHECK=1 DXMT_PRESENTER_FREE=$free ${OUT}_check_$arch; done; done
//
// gw2 (#184): a Metal 4 queue takes at most 32 residency sets. The presenter used to add its layer's
// set to the device's queue at its first present, to leave it in ~Presenter. But an Rc doesn't delete
// (src/util/rc/util_rc_ptr.hpp; a class that dies defines a decRef of its own) and Presenter has none,
// so no presenter was ever destroyed and every set stayed. Steam's browser makes 45 to 47 swap chains
// in a process's life; with the device's own set, the 32nd presenter's set ended it:
// "IOGPUMetal4CommandQueue: command queue  residency set limit of 32 exceeded". This test runs DXMT's
// own Presenter (src/dxmt/dxmt_presenter.cpp) on a real Metal 4 queue (gw2::Mtl4Queue,
// gw2::Mtl4Encoding) through the real bridge, as mtl4_queue_test and paired_replay_test do. No Wine,
// no game.
//
// The queue starts with one set of its own, as the device's set (#29) is on it in DXMT; the back
// buffers are in it. Each present is a chunk as the context encodes it: synchronizeLayerProperties,
// encodeCommandsMetal4 into the chunk's encoding, signalDrawable, present, then wait and retire. A swap
// chain is released as DXMT releases it: its layer goes and its Rc<Presenter> is dropped.
//
// What it checks:
// - 40 swap chains one after another, each made, presented with twice and released, as a program
//   that replaces its swap chain does;
// - 40 swap chains alive at once, each presented with, then each presented with again;
// - no command buffer reports an error (the arm64 run with MTL_DEBUG_LAYER and MTL_SHADER_VALIDATION).
//
// What it doesn't see: a present pass that names no set for its drawable. Without the layer's set the
// test still passes, with and without Metal's validation (2026-11-11, M5 Max), because the drawable is
// only a render attachment there. That rests on the presenter's code, not on this test.
//
// Both phases hold whether presenters stay or are deleted: the first fails only if a presenter's set
// stays on the queue after its swap chain, the second whenever each swap chain puts a set on the queue.
//
// gw2 (#185): an Rc doesn't delete, and Presenter used to inherit RcObject's decRef, which only
// counts down, so no presenter was ever deleted. Its own decRef deletes it at zero with
// DXMT_PRESENTER_FREE=1. The test counts presenters alive (Presenter::alive()) and runs the phases
// twice, with the switch off and on (the test's presenterFree() in place of the switch):
// - off (the default): the 40 replaced swap chains leave 40 presenters alive, as before #185;
// - on: they leave none, and the 40 alive at once are gone once released.
// Each present holds the presenter as the retirement ledger does (CommandQueue::Hold) and lets go
// once the chunk retired, after the present's PresentState went (CommandChunk::reset() destroys the
// present's command, then the ledger releases its holds).
//
// The lifetime-check build (${OUT}_check_*, -DDXMT_GW2_LIFETIME_CHECK=1 as meson's
// -Dgw2_lifetime_check=true) links the real check (src/dxmt/dxmt_gw2_lifetime.cpp) and the real
// DXMT_PRESENTER_FREE, read from the environment. Each present stamps the presenter's pipelines and
// gamma texture as the context does (gw2LifetimeUse), and each chunk is completed before it retires,
// as RetireChunk does. A presenter that died before its chunk completed would release them early;
// the check's LIFECHECK section must count 0. It runs with DXMT_LIFETIME_CHECK=1, once per value of
// the switch.
//
// gw2 (#227): DXMT_MTL4_PRESENT=mtl3. Both phases run a second time with each present through a Metal 3
// command buffer of a Metal 3 queue on the same device (the device's set on it too, as
// residencyAttachQueue does), the way the context encodes it: beginMetal3Present (the frame's commit and
// the `frame` event), Presenter::encodeCommands into the Metal 3 buffer, its presentDrawable,
// commitMetal3Present (`shown`, and the Metal 4 queue's wait for it). Mtl4Queue::wait waits for the
// Metal 3 buffer, which must complete without an error. Then 320 frames through that present, twice
// (with the frame's commit before the present, as DXMT_PRESENT_SPLIT does, and without it): each
// frame clears the back buffer to a colour of its own on the Metal 4 queue, and its present's Metal 3
// buffer reads the drawable back after the pass. Up to three chunks are in flight, so the next frame's
// clear is committed while the earlier pass may still wait. Every drawable must hold its own frame's
// colour: a pass that read a back buffer the next frame had already written shows the next colour.
// Whether this check catches a missing `shown` wait (delete the encodeWaitForEvent in
// Mtl4Encoding::commitMetal3Present in a scratch copy) is for the Mac run to show.
//
// What the test provides in place of DXMT: the statistics hooks (off) and clock, DXMT_PRESENT_QUEUE
// (default), the lifetime check
// (off) and DXMT_PRESENTER_FREE (set per phase) outside the lifetime-check build, the device set's
// commit hook (its set here is fixed), Forget_sRGB for the formats used here, and the internal
// command library, compiled at run time from src/dxmt/dxmt_command.metal (the build makes a metallib
// of the same file). Run it from the checkout's root, as tests/gw2/run.zsh does.
//
// Exit 77 (skipped): no Metal device, no Metal 4, or a layer without a window gave no drawable.

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "dxmt_presenter.hpp"
#include "dxmt_gw2_lifetime.hpp"
#include "dxmt_gw2_mtl4_encoding.hpp"
#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_stats.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                  \
      std::printf(__VA_ARGS__);                                                                    \
      std::printf("\n");                                                                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// --- what the rest of DXMT provides
namespace dxmt::gw2 {
static SectionFn g_lifecheck = nullptr; // the lifetime check's LIFECHECK section
void
registerSection(const char *name, SectionFn fn) {
  if (std::string(name) == "LIFECHECK")
    g_lifecheck = fn;
}
bool
statsEnabled() {
  return false;
}
// gw2 (#156): the presenter's layer poll
int64_t
nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void
residencyBeforeCommit() {}
// gw2 (#153): DXMT_MTL4_DEVICE_RESIDENCY unset (the default path)
bool
mtl4DeviceResidencyRequested() {
  return false;
}
// gw2 (#157): DXMT_PRESENT_QUEUE unset (the default path: the layer keeps its drawable count)
PresentQueue
presentQueue() {
  return PresentQueue::Default;
}
#ifndef DXMT_GW2_LIFETIME_CHECK
namespace lifetime {
void
useSlow(uint32_t, uint64_t, obj_handle_t, Kind) {}
void
completedSlow(uint32_t, uint64_t) {}
bool g_presenter_free = false; // DXMT_PRESENTER_FREE, per phase
bool
presenterFree() {
  return g_presenter_free;
}
} // namespace lifetime
#endif
} // namespace dxmt::gw2

namespace dxmt {
WMTPixelFormat
Forget_sRGB(WMTPixelFormat format) {
  return format == WMTPixelFormatBGRA8Unorm_sRGB ? WMTPixelFormatBGRA8Unorm : format;
}

static std::string
readFile(const char *path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

// as dxmt_command.cpp's, from the source instead of the build's metallib
InternalCommandLibrary::InternalCommandLibrary(WMT::Device device) {
  std::string source = readFile("src/dxmt/dxmt_command.metal");
  const std::string include = "#include \"dxmt_command_constants.hpp\"";
  auto at = source.find(include);
  if (at == std::string::npos) {
    std::printf("FAIL: src/dxmt/dxmt_command.metal not found or changed (run from the checkout's root)\n");
    std::exit(1);
  }
  source.replace(at, include.size(), readFile("src/dxmt/dxmt_command_constants.hpp"));
  NSError *error = nil;
  id<MTLDevice> mtl = (__bridge id<MTLDevice>)(void *)device.handle;
  id<MTLLibrary> library = [mtl newLibraryWithSource:@(source.c_str()) options:nil error:&error];
  if (!library) {
    std::printf("FAIL: dxmt_command.metal: %s\n", error.localizedDescription.UTF8String);
    std::exit(1);
  }
  library_ = WMT::Reference<WMT::Library>((obj_handle_t)(__bridge_retained void *)library);
}
} // namespace dxmt

using dxmt::Presenter;
using dxmt::Rc;
using dxmt::gw2::Mtl4Encoding;
using dxmt::gw2::Mtl4Queue;

template <typename T>
static obj_handle_t
H(T object) {
  return (obj_handle_t)(__bridge void *)object;
}

constexpr uint32_t kSize = 64;
constexpr uint32_t kSwapChains = 40; // more than a queue's 32 sets
constexpr uint32_t kSlots = 4;

// one swap chain: its window's layer, its back buffer and its presenter
struct SwapChain {
  CAMetalLayer *layer;
  id<MTLTexture> backbuffer;
  Rc<Presenter> presenter;
};

struct Fixture {
  id<MTLDevice> device;
  WMT::Device wmt_device;
  Mtl4Queue *queue;
  dxmt::InternalCommandLibrary *lib;
  id<MTLResidencySet> device_set; // as the device's set (#29): on the queue, with the back buffers
  id<MTLCommandQueue> mtl3;       // gw2 (#227): the command queue's MTLCommandQueue
  bool mtl3_present = false;      // gw2 (#227): DXMT_MTL4_PRESENT=mtl3
  std::vector<id<MTLBuffer>> arenas; // the chunks' bytes arenas
  uint32_t lt_queue = 0;              // the lifetime check's id of the queue (its build only)
  uint64_t seq = 0;
  uint32_t presents = 0;
};

static SwapChain
makeSwapChain(Fixture &fx) {
  SwapChain sc;
  sc.layer = [CAMetalLayer layer];
  sc.layer.device = fx.device;
  sc.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  sc.layer.drawableSize = CGSizeMake(kSize, kSize);
  MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:kSize
                                                                                 height:kSize
                                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  desc.storageMode = MTLStorageModePrivate;
  sc.backbuffer = [fx.device newTextureWithDescriptor:desc];
  [fx.device_set addAllocation:sc.backbuffer];
  [fx.device_set commit];
  sc.presenter = new Presenter(fx.wmt_device, WMT::MetalLayer{H(sc.layer)}, *fx.lib, 1.0f, 1);
  // as ResizeBuffers (ApplyLayerProps)
  sc.presenter->changeLayerProperties(WMTPixelFormatBGRA8Unorm, WMTColorSpaceSRGB, kSize, kSize, 1);
  return sc;
}

static void
releaseSwapChain(Fixture &fx, SwapChain &sc) {
  // as ~MTLD3D11SwapChain: the view (layer) goes, then the presenter (after its chunks retired)
  [fx.device_set removeAllocation:sc.backbuffer];
  [fx.device_set commit];
  sc.layer = nil;
  sc.presenter = nullptr;
  sc.backbuffer = nil;
}

// gw2 (#227): after wait(seq): the chunk's Metal 3 present buffers, `expected` of them, completed
static void
checkMetal3(Fixture &fx, uint64_t seq, size_t expected, const char *what, uint32_t index) {
  auto &presents = fx.queue->metal3Presents(seq);
  CHECK(presents.size() == expected, "%s %u: %zu Metal 3 present buffers, expected %zu", what, index,
        presents.size(), expected);
  for (auto &present : presents) {
    WMT::CommandBuffer cmdbuf = present.cmdbuf;
    CHECK(cmdbuf.status() == WMTCommandBufferStatusCompleted, "%s %u: the Metal 3 present's status is %u", what,
          index, unsigned(cmdbuf.status()));
  }
}

// one chunk with a present, as ArgumentEncodingContext encodes it on Metal 4 (the present split
// commits nothing here: the chunk has no other work), then waited for and retired
static void
present(Fixture &fx, SwapChain &sc, const char *what, uint32_t index) {
  uint64_t seq = ++fx.seq; // from 1, as the lifetime check's stamps count
  Rc<Presenter> held; // the retirement ledger's hold (CommandQueue::Hold), until the chunk retires
  {
    // the present's command (the swap chain's lambda) holds it until CommandChunk::reset()
    auto state = sc.presenter->synchronizeLayerProperties();
    {
      Mtl4Encoding encoding(*fx.queue, seq, [&](uint64_t size) {
        id<MTLBuffer> b = [fx.device newBufferWithLength:size
                                                 options:MTLResourceStorageModeShared |
                                                         MTLResourceHazardTrackingModeUntracked];
        fx.arenas.push_back(b);
        return Mtl4Encoding::Bytes{WMT::Buffer{H(b)}, 0, false};
      });
      if (fx.mtl3_present) {
        // gw2 (#227): as the context with DXMT_MTL4_PRESENT=mtl3 (CreateCommandBuffer: unretained)
        WMT::CommandBuffer cmdbuf{H([fx.mtl3 commandBufferWithUnretainedReferences])};
        uint64_t value = encoding.beginMetal3Present(cmdbuf);
        auto drawable = sc.presenter->encodeCommands(
            cmdbuf, WMT::Texture{H(sc.backbuffer)}, state.metadata, [](WMT::RenderCommandEncoder) {},
            [](WMT::RenderCommandEncoder) {}
        );
        cmdbuf.presentDrawable(drawable);
        encoding.commitMetal3Present(cmdbuf, value);
      } else {
        auto drawable = sc.presenter->encodeCommandsMetal4(
            encoding, WMT::Texture{H(sc.backbuffer)}, state.metadata, [](WMT::MTL4RenderCommandEncoder) {},
            [](WMT::MTL4RenderCommandEncoder) {}
        );
        encoding.signalDrawable(drawable); // commits the present pass first
        drawable.present();
      }
      sc.presenter->gw2LifetimeUse(fx.lt_queue, seq); // the context's stamp (lifetime-check build)
      held = sc.presenter;
      fx.presents++;
    }
    for (auto *commit : fx.queue->wait(seq))
      CHECK(!commit->feedback.error, "%s %u: the present's command buffer reported an error", what, index);
    checkMetal3(fx, seq, fx.mtl3_present ? 1 : 0, what, index);
    dxmt::gw2::lifetime::completed(fx.lt_queue, seq); // RetireChunk: first, before reset()
  }
  held = nullptr; // RetireChunk: after reset(), the ledger's holds
  fx.queue->retire(seq);
  fx.arenas.clear();
}

// the replaced and the alive-at-once phases, with DXMT_PRESENTER_FREE as `free`
static void
phases(Fixture &fx, bool free) {
#ifndef DXMT_GW2_LIFETIME_CHECK
  dxmt::gw2::lifetime::g_presenter_free = free;
#endif
  const char *mode = free ? "DXMT_PRESENTER_FREE=1" : "DXMT_PRESENTER_FREE=0";
  uint32_t before = Presenter::alive();

  // a program that replaces its swap chain: each made, presented with, released
  for (uint32_t i = 0; i < kSwapChains; i++) {
    @autoreleasepool {
      SwapChain sc = makeSwapChain(fx);
      present(fx, sc, "replaced swap chain", i);
      present(fx, sc, "replaced swap chain", i);
      releaseSwapChain(fx, sc);
    }
  }
  uint32_t left = Presenter::alive() - before;
  std::printf("%s: replaced %u swap chains one after another, %u presenters left alive\n", mode, kSwapChains,
              left);
  CHECK(left == (free ? 0 : kSwapChains), "%s: %u presenters left alive", mode, left);

  // a program with many windows: all alive at once
  before = Presenter::alive();
  std::vector<SwapChain> alive;
  for (uint32_t i = 0; i < kSwapChains; i++) {
    @autoreleasepool {
      alive.push_back(makeSwapChain(fx));
      present(fx, alive.back(), "alive swap chain", i);
    }
  }
  for (uint32_t i = 0; i < kSwapChains; i++) {
    @autoreleasepool {
      present(fx, alive[i], "alive swap chain, again", i);
    }
  }
  CHECK(Presenter::alive() - before == kSwapChains, "%s: %u presenters alive with %u swap chains", mode,
        Presenter::alive() - before, kSwapChains);
  for (auto &sc : alive)
    releaseSwapChain(fx, sc);
  alive.clear();
  left = Presenter::alive() - before;
  std::printf("%s: %u swap chains alive at once, %u presenters left alive once released\n", mode, kSwapChains,
              left);
  CHECK(left == (free ? 0 : kSwapChains), "%s: %u presenters left alive", mode, left);
}

// gw2 (#227): frame i's back buffer colour, BGRA8Unorm. Red cycles through three values 100 apart, so
// a frame and the two after it (the chunks in flight) differ by far more than sameColour's tolerance
static void
colour(uint32_t i, uint8_t (&bgra)[4]) {
  bgra[0] = 160;
  bgra[1] = uint8_t(8 + 2 * ((i / 3) % 120));
  bgra[2] = uint8_t(20 + 100 * (i % 3));
  bgra[3] = 255;
}

static bool
sameColour(const uint8_t *pixel, const uint8_t (&bgra)[4]) {
  for (int c = 0; c < 4; c++)
    if (std::abs(int(pixel[c]) - int(bgra[c])) > 1)
      return false;
  return true;
}

// gw2 (#227): 320 frames through the Metal 3 present on the Metal 4 queue (see the header). `split`:
// the frame's commit before the present, as DXMT_PRESENT_SPLIT=1 does; without it, beginMetal3Present
// commits it.
static void
colours(Fixture &fx, bool split) {
  constexpr uint32_t kFrames = 320;
  constexpr uint32_t kInFlight = kSlots - 1; // a ring slot is reused only after its chunk retired
  const char *mode = split ? "DXMT_MTL4_PRESENT=mtl3, DXMT_PRESENT_SPLIT=1" : "DXMT_MTL4_PRESENT=mtl3, DXMT_PRESENT_SPLIT=0";
  SwapChain sc = makeSwapChain(fx);
  std::vector<id<MTLBuffer>> readbacks;
  struct Pending {
    uint64_t seq;
    uint32_t frame;
    Rc<Presenter> held; // the retirement ledger's holds: the presenter and the drawable (the Metal 3
    WMT::Reference<WMT::MetalDrawable> drawable; // buffer is unretained, and the chunk outlives the pool)
  };
  std::deque<Pending> pending;
  auto retireOldest = [&] {
    auto &oldest = pending.front();
    for (auto *commit : fx.queue->wait(oldest.seq))
      CHECK(!commit->feedback.error, "%s %u: a Metal 4 command buffer reported an error", mode, oldest.frame);
    checkMetal3(fx, oldest.seq, 1, mode, oldest.frame);
    dxmt::gw2::lifetime::completed(fx.lt_queue, oldest.seq);
    oldest.held = nullptr;
    oldest.drawable = WMT::Reference<WMT::MetalDrawable>();
    fx.queue->retire(oldest.seq);
    pending.pop_front();
  };
  for (uint32_t i = 0; i < kFrames; i++) {
    @autoreleasepool {
      if (pending.size() == kInFlight)
        retireOldest();
      uint64_t seq = ++fx.seq;
      id<MTLBuffer> readback = [fx.device newBufferWithLength:kSize * kSize * 4 options:MTLResourceStorageModeShared];
      readbacks.push_back(readback);
      auto state = sc.presenter->synchronizeLayerProperties();
      Mtl4Encoding encoding(*fx.queue, seq, [&](uint64_t size) {
        id<MTLBuffer> b = [fx.device newBufferWithLength:size
                                                 options:MTLResourceStorageModeShared |
                                                         MTLResourceHazardTrackingModeUntracked];
        fx.arenas.push_back(b);
        return Mtl4Encoding::Bytes{WMT::Buffer{H(b)}, 0, false};
      });
      // the frame on the Metal 4 queue: its back buffer cleared to its colour
      uint8_t bgra[4];
      colour(i, bgra);
      WMTRenderPassInfo info;
      WMT::InitializeRenderPassInfo(info);
      info.colors[0].texture = H(sc.backbuffer);
      info.colors[0].load_action = WMTLoadActionClear;
      info.colors[0].store_action = WMTStoreActionStore;
      info.colors[0].clear_color = {bgra[2] / 255.0, bgra[1] / 255.0, bgra[0] / 255.0, 1.0};
      info.default_raster_sample_count = 1;
      info.render_target_width = kSize;
      info.render_target_height = kSize;
      auto clear = encoding.renderCommandEncoder(info);
      clear.endEncoding();
      if (split)
        encoding.commitOpen();
      // the present, as the context encodes it with DXMT_MTL4_PRESENT=mtl3
      id<MTLCommandBuffer> mtl3 = [fx.mtl3 commandBufferWithUnretainedReferences];
      WMT::CommandBuffer cmdbuf{H(mtl3)};
      uint64_t value = encoding.beginMetal3Present(cmdbuf);
      auto drawable = sc.presenter->encodeCommands(
          cmdbuf, WMT::Texture{H(sc.backbuffer)}, state.metadata, [](WMT::RenderCommandEncoder) {},
          [](WMT::RenderCommandEncoder) {}
      );
      CHECK(drawable.handle, "%s %u: no drawable", mode, i);
      if (!drawable.handle)
        break;
      // the test's own readback of the drawable, after the pass, in the present's buffer
      id<MTLBlitCommandEncoder> blit = [mtl3 blitCommandEncoder];
      [blit copyFromTexture:(__bridge id<MTLTexture>)(void *)drawable.texture().handle
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(kSize, kSize, 1)
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:kSize * 4
        destinationBytesPerImage:kSize * kSize * 4];
      [blit endEncoding];
      cmdbuf.presentDrawable(drawable);
      encoding.commitMetal3Present(cmdbuf, value);
      sc.presenter->gw2LifetimeUse(fx.lt_queue, seq);
      pending.push_back({seq, i, sc.presenter, WMT::Reference<WMT::MetalDrawable>(drawable)});
      fx.presents++;
    }
  }
  while (!pending.empty())
    retireOldest();
  fx.arenas.clear();

  uint32_t wrong = 0, next = 0;
  for (uint32_t i = 0; i < readbacks.size(); i++) {
    auto *pixels = static_cast<const uint8_t *>(readbacks[i].contents);
    uint8_t own[4], following[4], after[4];
    colour(i, own);
    colour(i + 1, following);
    colour(i + 2, after);
    // the check itself: a drawable holding a later frame's colour must fail it
    CHECK(!sameColour(following, own) && !sameColour(after, own), "%s: frame %u's colour matches a later frame's",
          mode, i);
    uint32_t bad = 0, as_next = 0;
    for (uint32_t p = 0; p < kSize * kSize; p++) {
      if (!sameColour(pixels + p * 4, own)) {
        bad++;
        if (sameColour(pixels + p * 4, following))
          as_next++;
      }
    }
    if (bad) {
      if (wrong < 8)
        std::printf("%s: frame %u: %u of %u pixels aren't its colour, %u are the next frame's (first: %u %u %u %u)\n",
                    mode, i, bad, kSize * kSize, as_next, pixels[0], pixels[1], pixels[2], pixels[3]);
      wrong++;
    }
    if (as_next)
      next++;
  }
  std::printf("%s: %zu frames, %u with a wrong colour, %u of them with the next frame's\n", mode, readbacks.size(),
              wrong, next);
  CHECK(readbacks.size() == kFrames, "%s: %zu of %u frames presented", mode, readbacks.size(), kFrames);
  CHECK(wrong == 0, "%s: %u frames read a back buffer that wasn't theirs (%u the next frame's)", mode, wrong, next);
  releaseSwapChain(fx, sc);
}

int
main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0); // what passed shows before an assertion aborts the run
  @autoreleasepool {
    Fixture fx;
    fx.device = MTLCreateSystemDefaultDevice();
    if (!fx.device) {
      std::printf("skip: no Metal device\n");
      return 77;
    }
    fx.wmt_device = WMT::Device{H(fx.device)};
    {
      // the presenter has no path without a drawable: check that a windowless layer gives one
      CAMetalLayer *probe = [CAMetalLayer layer];
      probe.device = fx.device;
      probe.drawableSize = CGSizeMake(kSize, kSize);
      if (![probe nextDrawable]) {
        std::printf("skip: a layer without a window gave no drawable\n");
        return 77;
      }
    }
    auto support = MTLDevice_metal4Support(fx.wmt_device.handle);
    if (support != WMTMetal4Supported) {
      std::printf("skip: no Metal 4 (%u)\n", unsigned(support));
      return 77;
    }
    std::printf("device: %s (%s)\n", fx.device.name.UTF8String,
#if defined(__x86_64__)
                "x86_64, Rosetta"
#else
                "arm64"
#endif
    );
    dxmt::InternalCommandLibrary lib(fx.wmt_device);
    fx.lib = &lib;
    Mtl4Queue queue(fx.wmt_device, kSlots);
    CHECK(queue.valid(), "Metal 4 queue");
    if (!queue.valid())
      return 1;
    fx.queue = &queue;
    MTLResidencySetDescriptor *set_desc = [MTLResidencySetDescriptor new];
    fx.device_set = [fx.device newResidencySetWithDescriptor:set_desc error:nil];
    queue.queue().addResidencySet(WMT::ResidencySet{H(fx.device_set)});
    // gw2 (#227): DXMT_MTL4_PRESENT=mtl3's events, and the Metal 3 queue with the device's set
    CHECK(queue.enableMetal3Present(), "the Metal 3 present's events");
    fx.mtl3 = [fx.device newCommandQueue];
    [fx.mtl3 addResidencySet:fx.device_set];

#ifdef DXMT_GW2_LIFETIME_CHECK
    CHECK(dxmt::gw2::lifetime::check(), "DXMT_LIFETIME_CHECK=1 in the lifetime-check build");
    CHECK(dxmt::gw2::g_lifecheck, "the lifetime check registered its LIFECHECK section");
    fx.lt_queue = dxmt::gw2::lifetime::attachQueue();
    for (bool mtl3 : {false, true}) { // gw2 (#227): DXMT_MTL4_PRESENT=mtl4, then mtl3
      fx.mtl3_present = mtl3;
      std::printf("DXMT_MTL4_PRESENT=%s\n", mtl3 ? "mtl3" : "mtl4");
      phases(fx, dxmt::gw2::lifetime::presenterFree()); // the switch, from the environment
    }
#else
    for (bool mtl3 : {false, true}) { // gw2 (#227): DXMT_MTL4_PRESENT=mtl4, then mtl3
      fx.mtl3_present = mtl3;
      std::printf("DXMT_MTL4_PRESENT=%s\n", mtl3 ? "mtl3" : "mtl4");
      phases(fx, false); // the default: presenters stay, as before #185
      phases(fx, true);
    }
#endif
    // gw2 (#227): the colour check, with the frame's commit before the present and without
    colours(fx, true);
    colours(fx, false);
    queue.waitIdle();
#ifdef DXMT_GW2_LIFETIME_CHECK
    if (dxmt::gw2::g_lifecheck) {
      std::string line = dxmt::gw2::g_lifecheck(0, 0);
      std::printf("LIFECHECK %s\n", line.c_str());
      CHECK(line.find(" total=0") != std::string::npos, "an object released before its chunk completed");
    }
    dxmt::gw2::lifetime::detachQueue(fx.lt_queue);
#endif

    std::printf("presents %u\n", fx.presents);
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
