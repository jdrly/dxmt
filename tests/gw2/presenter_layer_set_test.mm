// gw2-test: metal
// gw2-build: U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
// gw2-build: (( ${#U} >= 5 ))
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   for f in $U; do clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Wno-unused-function -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $T/u_${f:r}_$arch.o; done
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal src/winemetal/winemetal_thunks.c -o $T/thunks_$arch.o
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c -o $T/airconv_absent_$arch.o
// gw2-build:   clang++ -std=c++20 -fobjc-arc -Wall -arch $arch -mmacosx-version-min=26.0 -DDXMT_PAGE_SIZE=4096 -Itests/gw2/shim -Isrc/winemetal -Isrc/dxmt -Isrc/util -Iinclude tests/gw2/presenter_layer_set_test.mm src/dxmt/dxmt_presenter.cpp src/dxmt/dxmt_gw2_mtl4_queue.cpp src/dxmt/dxmt_gw2_mtl4_encoding.cpp $T/thunks_$arch.o $T/u_*_$arch.o $T/airconv_absent_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -o ${OUT}_$arch
// gw2-build: done
// gw2-run: ${OUT}_x86_64 && MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 ${OUT}_arm64
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
// Both phases hold whether presenters leak (today) or are deleted: the first fails only if a
// presenter's set stays on the queue after its swap chain, the second whenever each swap chain puts a
// set on the queue.
//
// What the test provides in place of DXMT: the statistics hooks (off), the lifetime check (off), the
// device set's commit hook (its set here is fixed), Forget_sRGB for the formats used here, and the
// internal command library, compiled at run time from src/dxmt/dxmt_command.metal (the build makes
// a metallib of the same file). Run it from the checkout's root, as tests/gw2/run.zsh does.
//
// Exit 77 (skipped): no Metal device, no Metal 4, or a layer without a window gave no drawable.

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "dxmt_presenter.hpp"
#include "dxmt_gw2_lifetime.hpp"
#include "dxmt_gw2_mtl4_encoding.hpp"
#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_stats.hpp"
#include <cstdio>
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
void
registerSection(const char *, SectionFn) {}
bool
statsEnabled() {
  return false;
}
void
residencyBeforeCommit() {}
namespace lifetime {
void
useSlow(uint32_t, uint64_t, obj_handle_t, Kind) {}
} // namespace lifetime
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
  std::vector<id<MTLBuffer>> arenas; // the chunks' bytes arenas
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

// one chunk with a present, as ArgumentEncodingContext encodes it on Metal 4 (the present split
// commits nothing here: the chunk has no other work), then waited for and retired
static void
present(Fixture &fx, SwapChain &sc, const char *what, uint32_t index) {
  auto state = sc.presenter->synchronizeLayerProperties();
  uint64_t seq = fx.seq++;
  {
    Mtl4Encoding encoding(*fx.queue, seq, [&](uint64_t size) {
      id<MTLBuffer> b = [fx.device newBufferWithLength:size
                                               options:MTLResourceStorageModeShared |
                                                       MTLResourceHazardTrackingModeUntracked];
      fx.arenas.push_back(b);
      return std::pair<WMT::Buffer, uint64_t>{WMT::Buffer{H(b)}, 0};
    });
    auto drawable = sc.presenter->encodeCommandsMetal4(
        encoding, WMT::Texture{H(sc.backbuffer)}, state.metadata, [](WMT::MTL4RenderCommandEncoder) {},
        [](WMT::MTL4RenderCommandEncoder) {}
    );
    encoding.signalDrawable(drawable); // commits the present pass first
    drawable.present();
    fx.presents++;
  }
  for (auto *commit : fx.queue->wait(seq))
    CHECK(!commit->feedback.error, "%s %u: the present's command buffer reported an error", what, index);
  fx.queue->retire(seq);
  fx.arenas.clear();
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

    // a program that replaces its swap chain: each made, presented with, released
    for (uint32_t i = 0; i < kSwapChains; i++) {
      @autoreleasepool {
        SwapChain sc = makeSwapChain(fx);
        present(fx, sc, "replaced swap chain", i);
        present(fx, sc, "replaced swap chain", i);
        releaseSwapChain(fx, sc);
      }
    }
    std::printf("replaced: %u swap chains, one after another\n", kSwapChains);

    // a program with many windows: all alive at once
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
    std::printf("alive: %u swap chains at once\n", kSwapChains);
    for (auto &sc : alive)
      releaseSwapChain(fx, sc);
    alive.clear();
    queue.waitIdle();

    std::printf("presents %u\n", fx.presents);
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
