// gw2 (#32): native test of DXMT's Metal 4 submission (src/dxmt/dxmt_gw2_mtl4_queue.cpp, jdrly/dxmt
// #32 part 2) through the real bridge: the PE side (winemetal_thunks.c, Metal.hpp) compiled natively,
// its unix calls dispatched straight to the unix side (src/winemetal/unix/mtl4.c, and a few Metal 3
// calls written here) instead of through Wine. A real Metal 4 queue; no Wine, no game. Not part of
// the build; run it on the Mac:
//
//   T=$(mktemp -d); for arch in x86_64 arm64; do
//     clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Isrc/winemetal -Itests/gw2/mtl4_shim \
//       src/winemetal/winemetal_thunks.c -o $T/thunks_$arch.o &&
//     clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -arch $arch -mmacosx-version-min=26.0 \
//       -Isrc/winemetal -Isrc/winemetal/unix -Isrc/airconv src/winemetal/unix/mtl4.c -o $T/mtl4_$arch.o &&
//     clang++ -std=c++20 -fobjc-arc -arch $arch -mmacosx-version-min=26.0 -Itests/gw2/shim \
//       -Itests/gw2/mtl4_shim -Isrc/winemetal -Isrc/winemetal/unix -Isrc/dxmt \
//       tests/gw2/mtl4_queue_test.mm src/dxmt/dxmt_gw2_mtl4_queue.cpp $T/thunks_$arch.o $T/mtl4_$arch.o \
//       -framework Metal -framework QuartzCore -framework Foundation -o $T/mtl4_queue_test_$arch
//   done
//   $T/mtl4_queue_test_x86_64 && MTL_DEBUG_LAYER=1 $T/mtl4_queue_test_arm64
//
// What it checks:
// - a ring of 4 slots for 24 chunks, with a finish thread as DXMT's: each chunk fills its own bytes
//   of a buffer with its sequence number (a blit replay on an MTL4 compute encoder), some chunks
//   commit twice (as the present split does); the finish thread's wait() returns every commit done,
//   without an error, with GPU times, and the GPU wrote the value; retire() resets the slot's
//   allocator before the slot is used again;
// - a queue-level waitForEvent holds the next commit until the CPU signals the event;
// - waitIdle() and the destructor with nothing pending.

#import <Metal/Metal.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_stats.hpp"
extern "C" {
#include "mtl4.h"
#include "winemetal_thunks.h"
}

using dxmt::gw2::Mtl4Commit;
using dxmt::gw2::Mtl4Queue;

static std::atomic<int> failures{0};
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                  \
      std::printf(__VA_ARGS__);                                                                    \
      std::printf("\n");                                                                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// --- what DXMT's statistics module provides
namespace dxmt::gw2 {
void
registerSection(const char *, SectionFn) {}
bool
statsEnabled() {
  return true;
}
} // namespace dxmt::gw2

// --- the unix side of the calls this test makes, by bridge slot
#define OBJ(h) ((__bridge id)(void *)(uintptr_t)(h))
static NTSTATUS
u_retain(void *p) {
  CFRetain((CFTypeRef)(uintptr_t) * (obj_handle_t *)p);
  return 0;
}
static NTSTATUS
u_release(void *p) {
  CFRelease((CFTypeRef)(uintptr_t) * (obj_handle_t *)p);
  return 0;
}
static NTSTATUS
u_newSharedEvent(void *p) {
  auto *params = (unixcall_generic_obj_obj_ret *)p;
  params->ret = (obj_handle_t)(uintptr_t)CFBridgingRetain([(id<MTLDevice>)OBJ(params->handle) newSharedEvent]);
  return 0;
}
static NTSTATUS
u_signaledValue(void *p) {
  auto *params = (unixcall_generic_obj_uint64_ret *)p;
  params->ret = [(id<MTLSharedEvent>)OBJ(params->handle) signaledValue];
  return 0;
}
static NTSTATUS
u_waitUntilSignaledValue(void *p) {
  auto *params = (unixcall_mtlsharedevent_waituntilsignaledvalue *)p;
  params->ret_timeout = [(id<MTLSharedEvent>)OBJ(params->event) waitUntilSignaledValue:params->value
                                                                              timeoutMS:params->timeout_ms];
  return 0;
}
static NTSTATUS
u_endEncoding(void *p) {
  [(id<MTLCommandEncoder>)OBJ(((unixcall_generic_obj_noret *)p)->handle) endEncoding];
  return 0;
}

static NTSTATUS (*g_table[190])(void *);

extern "C" {
uint64_t __wine_unixlib_handle = 0;
static NTSTATUS
dispatch(uint64_t, unsigned int code, void *args) {
  if (code >= 190 || !g_table[code]) {
    std::fprintf(stderr, "unix call %u isn't in the test's table\n", code);
    std::abort();
  }
  return g_table[code](args);
}
NTSTATUS (*__wine_unix_call_dispatcher)(uint64_t, unsigned int, void *) = dispatch;
}

static void
fillTable() {
  g_table[0] = u_retain;
  g_table[1] = u_release;
  g_table[15] = u_newSharedEvent;
  g_table[16] = u_signaledValue;
  g_table[33] = u_endEncoding;
  g_table[126] = u_waitUntilSignaledValue;
  NTSTATUS (*mtl4[])(void *) = {
      _MTLDevice_metal4Support,
      _MTLDevice_newMTL4CommandQueue,
      _MTLDevice_newCommandAllocator,
      _MTLDevice_newMTL4CommandBuffer,
      _MTLDevice_newArgumentTable,
      _MTL4CommandAllocator_reset,
      _MTL4CommandBuffer_beginCommandBuffer,
      _MTL4CommandBuffer_endCommandBuffer,
      _MTL4CommandBuffer_useResidencySet,
      _MTL4CommandQueue_commit,
      _MTL4CommandQueue_waitForDrawable,
      _MTL4CommandQueue_signalDrawable,
      _MTL4CommandQueue_signalEvent,
      _MTL4CommandQueue_waitForEvent,
      _MTL4CommandQueue_addResidencySet,
      _MTL4CommandQueue_removeResidencySet,
      _MetalLayer_residencySet,
      _MetalDrawable_present,
      _MTL4CommandBuffer_renderCommandEncoder,
      _MTL4CommandBuffer_computeCommandEncoder,
      _MTL4RenderCommandEncoder_encodeCommands,
      _MTL4ComputeCommandEncoder_encodeCommands,
      _MTL4ComputeCommandEncoder_encodeBlitCommands,
      _MTL4CommandEncoder_barrier,
  };
  for (unsigned i = 0; i < sizeof(mtl4) / sizeof(mtl4[0]); i++)
    g_table[155 + i] = mtl4[i];
}

// one chunk's GPU work: fill its 4 bytes of `buffer` with seq
static void
encodeFill(Mtl4Commit &commit, id<MTLBuffer> buffer, uint64_t seq, uint32_t slots) {
  WMTMtl4BindingState state{};
  auto encoder = WMT::MTL4CommandBuffer{commit.cmdbuf.handle}.computeCommandEncoder(&state);
  wmtcmd_blit_fillbuffer fill{};
  fill.type = WMTBlitCommandFillBuffer;
  fill.next.set(nullptr);
  fill.buffer = (obj_handle_t)(uintptr_t)(__bridge void *)buffer;
  fill.offset = (seq % slots) * 4;
  fill.length = 4;
  fill.value = uint8_t(seq);
  encoder.encodeBlitCommands((const wmtcmd_blit_nop *)&fill);
  encoder.endEncoding();
}

int
main() {
  fillTable();
  @autoreleasepool {
    id<MTLDevice> mtl_device = MTLCreateSystemDefaultDevice();
    WMT::Device device{(obj_handle_t)(uintptr_t)(__bridge void *)mtl_device};
    if (device.metal4Support() != WMTMetal4Supported) {
      std::printf("no Metal 4 here: nothing to test\n");
      return 1;
    }
    constexpr uint32_t kSlots = 4;
    constexpr uint64_t kChunks = 24;
    id<MTLBuffer> buffer = [mtl_device newBufferWithLength:4 * kSlots
                                                   options:MTLResourceStorageModeShared |
                                                           MTLResourceHazardTrackingModeUntracked];
    id<MTLSharedEvent> external = [mtl_device newSharedEvent];
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
    NSError *err = nil;
    id<MTLResidencySet> residency = [mtl_device newResidencySetWithDescriptor:rd error:&err];
    [residency addAllocation:buffer];
    [residency commit];
    {
      Mtl4Queue queue(device, kSlots);
      CHECK(queue.valid(), "Mtl4Queue isn't valid");
      if (!queue.valid())
        return 1;
      queue.queue().addResidencySet(WMT::ResidencySet{(obj_handle_t)(uintptr_t)(__bridge void *)residency});

      // the ring: chunk seq may begin once chunk seq - slots + 1 retired (DXMT keeps one slot free)
      std::atomic<uint64_t> committed{0}, retired{0};
      std::thread finish([&] {
        for (uint64_t seq = 1; seq <= kChunks; seq++) {
          while (committed.load(std::memory_order_acquire) < seq)
            std::this_thread::yield();
          auto &commits = queue.wait(seq);
          size_t want = seq % 3 == 0 ? 2 : 1;
          CHECK(commits.size() == want, "chunk %llu: %zu commits, want %zu", (unsigned long long)seq, commits.size(),
                want);
          uint64_t last_end = 0;
          for (auto *commit : commits) {
            CHECK(commit->done(), "chunk %llu: a commit not done after wait()", (unsigned long long)seq);
            CHECK(!commit->feedback.error, "chunk %llu: GPU error", (unsigned long long)seq);
            CHECK(commit->gpuStartNs() > 0 && commit->gpuEndNs() >= commit->gpuStartNs(),
                  "chunk %llu: GPU times %llu-%llu", (unsigned long long)seq,
                  (unsigned long long)commit->gpuStartNs(), (unsigned long long)commit->gpuEndNs());
            CHECK(commit->gpuEndNs() >= last_end, "chunk %llu: commits completed out of order",
                  (unsigned long long)seq);
            last_end = commit->gpuEndNs();
          }
          const uint8_t *bytes = (const uint8_t *)buffer.contents + (seq % kSlots) * 4;
          CHECK(bytes[0] == uint8_t(seq) && bytes[3] == uint8_t(seq), "chunk %llu: the GPU wrote %u, want %u",
                (unsigned long long)seq, bytes[0], unsigned(uint8_t(seq)));
          queue.retire(seq);
          retired.store(seq, std::memory_order_release);
        }
      });

      for (uint64_t seq = 1; seq <= kChunks; seq++) {
        while (seq > retired.load(std::memory_order_acquire) + kSlots - 1)
          std::this_thread::yield();
        if (seq % 3 == 0) {
          // as the present split: the frame's part first, then the rest in a second buffer
          auto &first = queue.begin(seq);
          encodeFill(first, buffer, seq, kSlots);
          queue.commit(seq);
          CHECK(queue.current(seq) == nullptr, "chunk %llu: still open after commit", (unsigned long long)seq);
        }
        auto &commit = queue.begin(seq);
        CHECK(queue.current(seq) == &commit, "chunk %llu: current() isn't the open commit", (unsigned long long)seq);
        encodeFill(commit, buffer, seq, kSlots);
        if (seq == 10) {
          // the GPU must hold this chunk's commit until the CPU signals `external`
          queue.waitForEvent(WMT::Event{(obj_handle_t)(uintptr_t)(__bridge void *)external}, 1);
          queue.commit(seq);
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          CHECK(!commit.done(), "chunk 10 completed before its queue-level event wait was satisfied");
          external.signaledValue = 1;
        } else {
          queue.commit(seq);
        }
        committed.store(seq, std::memory_order_release);
      }
      finish.join();
      queue.waitIdle();
    }
    std::printf("Mtl4Queue: %llu chunks on %u slots, split commits, a queue-level event wait: %s\n",
                (unsigned long long)kChunks, kSlots, failures ? "FAILED" : "ok");
  }
  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
