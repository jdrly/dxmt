// gw2-test: metal
// gw2-build: U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
// gw2-build: (( ${#U} >= 5 ))
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   for f in $U; do clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Wno-unused-function -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $T/u_${f:r}_$arch.o; done
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal src/winemetal/winemetal_thunks.c -o $T/thunks_$arch.o
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c -o $T/airconv_absent_$arch.o
// gw2-build:   clang++ -std=c++20 -fobjc-arc -arch $arch -mmacosx-version-min=26.0 -Itests/gw2/shim -Isrc/winemetal -Isrc/dxmt tests/gw2/mtl4_queue_test.mm src/dxmt/dxmt_gw2_mtl4_queue.cpp $T/thunks_$arch.o $T/u_*_$arch.o $T/airconv_absent_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -o ${OUT}_$arch
// gw2-build: done
// gw2-run: ${OUT}_x86_64 && MTL_DEBUG_LAYER=1 ${OUT}_arm64
//
// gw2 (#32): native test of DXMT's Metal 4 submission (src/dxmt/dxmt_gw2_mtl4_queue.cpp, jdrly/dxmt
// #32 part 2) through the real bridge: the PE side (winemetal_thunks.c, Metal.hpp) compiled natively,
// its bridge calls going through the real table, as Wine's would. A real Metal 4 queue; no Wine, no
// game. Run it with `tests/gw2/run.zsh metal`.
//
// gw2 (B3, jdrly/dxmt #76): on the real table. The build lines link the whole unix side (every source
// in src/winemetal/unix/meson.build's winemetal_unix_src, read from that list) with DXMT_NATIVE, so
// each call reaches __wine_unix_call_funcs through wmt_native_dispatch (src/nativemetal/wineunixlib.h)
// by its manifest number. Only airconv, the shader compiler, is left out (tests/gw2/shim/airconv_absent.c);
// this test compiles no shader. Before B3 it dispatched through a hand-written table of its own.
//
// The fill buffer isn't in any set of the test's own: the blit replay puts it into the chunk slot's
// residency set (part 3), which each commit commits and uses and retire() empties.
//
// What it checks:
// - a ring of 4 slots for 24 chunks, with a finish thread as DXMT's: each chunk fills its own bytes
//   of a buffer with its sequence number (a blit replay on an MTL4 compute encoder), some chunks
//   commit twice (as the present split does); the finish thread's wait() returns every commit done,
//   without an error, with GPU times, and the GPU wrote the value; retire() resets the slot's
//   allocator before the slot is used again;
// - a queue-level waitForEvent holds the next commit until the CPU signals the event;
// - gw2 (#153): retire() leaves the slot's set empty; a chunk whose set stays empty (a command buffer
//   that names nothing) completes without an error and leaves it empty, both on the default path
//   (the set committed and used anyway) and with DXMT_MTL4_DEVICE_RESIDENCY=1 (committed without it);
// - waitIdle() and the destructor with nothing pending;
// - gw2 (#204): 100 chunks through a ring of 32 with at most 3 in flight (the finish thread retires a
//   chunk only once the next two are committed, so 3 are in flight from chunk 3 on), with
//   DXMT_MTL4_ALLOCATOR_POOL on and off. On: every command buffer completes, an allocator serves one
//   unretired chunk at a time (so it is reset only after the one chunk that used it completed), a
//   chunk's command buffers share one, and the queue ends with 3 or 4 allocators alive; after 12
//   chunks in flight at once, with 8 (the most kept idle). Off: 32, one per slot, as before.

#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_stats.hpp"
extern "C" {
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
// the device set (#29) isn't in this test: the queue's own per-slot sets are
void
residencyBeforeCommit() {}
// gw2 (#153): DXMT_MTL4_DEVICE_RESIDENCY, unset (the default path) except where main sets it
static bool g_device_residency = false;
bool
mtl4DeviceResidencyRequested() {
  return g_device_residency;
}
} // namespace dxmt::gw2

static void encodeFill(Mtl4Queue &queue, Mtl4Commit &commit, id<MTLBuffer> buffer, uint64_t seq, uint32_t slots);

// gw2 (#204): the allocator pool's case (see the header), on or off
static void
allocatorPoolCase(id<MTLDevice> mtl_device, WMT::Device device, bool pool) {
  constexpr uint32_t kSlots = 32;
  constexpr uint64_t kChunks = 100;
  constexpr uint64_t kInFlight = 3;
  const char *mode = pool ? "pool" : "per slot";
  id<MTLBuffer> buffer = [mtl_device newBufferWithLength:4 * kSlots
                                                 options:MTLResourceStorageModeShared |
                                                         MTLResourceHazardTrackingModeUntracked];
  Mtl4Queue queue(device, kSlots, pool);
  CHECK(queue.valid(), "%s: Mtl4Queue isn't valid", mode);
  if (!queue.valid())
    return;
  CHECK(queue.allocators() == (pool ? 1u : kSlots), "%s: %u allocators after construction", mode,
        queue.allocators());

  // which unretired chunk each allocator serves (both threads)
  std::mutex owners_mutex;
  std::map<obj_handle_t, uint64_t> owners;
  uint32_t most_alive = 0;
  std::atomic<uint64_t> committed{0}, retired{0};
  auto retireChunk = [&](uint64_t seq) {
    auto &commits = queue.wait(seq);
    for (auto *commit : commits)
      CHECK(commit->done() && !commit->feedback.error, "%s: chunk %llu: a commit not done or failed", mode,
            (unsigned long long)seq);
    const uint8_t *bytes = (const uint8_t *)buffer.contents + (seq % kSlots) * 4;
    CHECK(bytes[0] == uint8_t(seq), "%s: chunk %llu: the GPU wrote %u", mode, (unsigned long long)seq, bytes[0]);
    obj_handle_t used = queue.allocator(seq).handle;
    {
      std::lock_guard lock(owners_mutex);
      if (pool) {
        // retire() resets it now: the chunk it serves must be this one, every commit of it done
        CHECK(owners.count(used) && owners[used] == seq, "%s: chunk %llu: its allocator serves another chunk", mode,
              (unsigned long long)seq);
        owners.erase(used);
      }
    }
    queue.retire(seq);
    if (pool)
      CHECK(!queue.allocator(seq), "%s: chunk %llu kept its allocator after retire()", mode,
            (unsigned long long)seq);
  };
  auto beginChunk = [&](uint64_t seq) {
    for (int part = 0; part < (seq % 3 == 0 ? 2 : 1); part++) {
      auto &commit = queue.begin(seq);
      obj_handle_t used = queue.allocator(seq).handle;
      CHECK(used, "%s: chunk %llu began without an allocator", mode, (unsigned long long)seq);
      {
        std::lock_guard lock(owners_mutex);
        if (pool && part == 0) {
          CHECK(!owners.count(used), "%s: chunk %llu took the allocator of unretired chunk %llu", mode,
                (unsigned long long)seq, (unsigned long long)owners[used]);
          owners[used] = seq;
        }
        if (pool && part == 1)
          CHECK(owners[used] == seq, "%s: chunk %llu's second command buffer is on another allocator", mode,
                (unsigned long long)seq);
      }
      most_alive = std::max(most_alive, queue.allocators());
      encodeFill(queue, commit, buffer, seq, kSlots);
      queue.commit(seq);
    }
  };

  std::thread finish([&] {
    for (uint64_t seq = 1; seq <= kChunks; seq++) {
      // three in flight: chunk seq retires once seq + 2 is committed (or the last one is)
      while (committed.load(std::memory_order_acquire) < std::min(seq + kInFlight - 1, kChunks))
        std::this_thread::yield();
      retireChunk(seq);
      retired.store(seq, std::memory_order_release);
    }
  });
  for (uint64_t seq = 1; seq <= kChunks; seq++) {
    while (seq > retired.load(std::memory_order_acquire) + kInFlight)
      std::this_thread::yield();
    beginChunk(seq);
    committed.store(seq, std::memory_order_release);
  }
  finish.join();
  uint32_t alive = queue.allocators();
  CHECK(pool ? alive == kInFlight || alive == kInFlight + 1 : alive == kSlots,
        "%s: %u allocators alive after %llu chunks, %llu in flight at most (most seen %u)", mode, alive,
        (unsigned long long)kChunks, (unsigned long long)kInFlight, most_alive);
  CHECK(queue.allocatorBytes() > 0, "%s: allocheld is 0 after %llu chunks", mode, (unsigned long long)kChunks);
  std::printf("allocators, %s: %u alive after %llu chunks on %u slots with %llu in flight, %.2f MB held\n", mode,
              queue.allocators(), (unsigned long long)kChunks, kSlots, (unsigned long long)kInFlight,
              queue.allocatorBytes() / 1e6);

  // 12 chunks in flight at once, then retired: the pool keeps 8 of the 12 allocators
  constexpr uint64_t kBurst = 12;
  for (uint64_t seq = kChunks + 1; seq <= kChunks + kBurst; seq++)
    beginChunk(seq);
  CHECK(!pool || queue.allocators() == kBurst, "%s: %u allocators alive with %llu chunks in flight", mode,
        queue.allocators(), (unsigned long long)kBurst);
  for (uint64_t seq = kChunks + 1; seq <= kChunks + kBurst; seq++)
    retireChunk(seq);
  uint32_t want = pool ? uint32_t(Mtl4Queue::kIdleAllocators) : kSlots;
  CHECK(queue.allocators() == want, "%s: %u allocators alive after the burst retired, want %u", mode,
        queue.allocators(), want);
  queue.waitIdle();
}

// one chunk's GPU work: fill its 4 bytes of `buffer` with seq
static void
encodeFill(Mtl4Queue &queue, Mtl4Commit &commit, id<MTLBuffer> buffer, uint64_t seq, uint32_t slots) {
  WMTMtl4BindingState state{};
  state.residency_set = queue.residency(seq).handle;
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
  @autoreleasepool {
    id<MTLDevice> mtl_device = MTLCreateSystemDefaultDevice();
    if (!mtl_device) {
      std::printf("no Metal device: skipped\n");
      return 77; // X1: skipped, not passed
    }
    WMT::Device device{(obj_handle_t)(uintptr_t)(__bridge void *)mtl_device};
    if (device.metal4Support() != WMTMetal4Supported) {
      std::printf("no Metal 4 here: skipped\n");
      return 77; // X1: skipped, not passed
    }
    constexpr uint32_t kSlots = 4;
    constexpr uint64_t kChunks = 24;
    id<MTLBuffer> buffer = [mtl_device newBufferWithLength:4 * kSlots
                                                   options:MTLResourceStorageModeShared |
                                                           MTLResourceHazardTrackingModeUntracked];
    id<MTLSharedEvent> external = [mtl_device newSharedEvent];
    {
      Mtl4Queue queue(device, kSlots);
      CHECK(queue.valid(), "Mtl4Queue isn't valid");
      if (!queue.valid())
        return 1;

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
          // no order between a chunk's commits is checked: Metal 4 may run separately committed
          // command buffers concurrently (measured: the second of a split chunk ended first under
          // Rosetta), and DXMT orders dependent work across them with fences, not by queue order
          // (mtl4_bridge_test.m's cross-commit fence check)
          for (auto *commit : commits) {
            CHECK(commit->done(), "chunk %llu: a commit not done after wait()", (unsigned long long)seq);
            CHECK(!commit->feedback.error, "chunk %llu: GPU error", (unsigned long long)seq);
            CHECK(commit->gpuStartNs() > 0 && commit->gpuEndNs() >= commit->gpuStartNs(),
                  "chunk %llu: GPU times %llu-%llu", (unsigned long long)seq,
                  (unsigned long long)commit->gpuStartNs(), (unsigned long long)commit->gpuEndNs());
          }
          const uint8_t *bytes = (const uint8_t *)buffer.contents + (seq % kSlots) * 4;
          CHECK(bytes[0] == uint8_t(seq) && bytes[3] == uint8_t(seq), "chunk %llu: the GPU wrote %u, want %u",
                (unsigned long long)seq, bytes[0], unsigned(uint8_t(seq)));
          queue.retire(seq);
          CHECK(queue.residency(seq).allocationCount() == 0, "chunk %llu: the slot's set isn't empty after retire()",
                (unsigned long long)seq);
          retired.store(seq, std::memory_order_release);
        }
      });

      for (uint64_t seq = 1; seq <= kChunks; seq++) {
        while (seq > retired.load(std::memory_order_acquire) + kSlots - 1)
          std::this_thread::yield();
        if (seq % 3 == 0) {
          // as the present split: the frame's part first, then the rest in a second buffer
          auto &first = queue.begin(seq);
          encodeFill(queue, first, buffer, seq, kSlots);
          queue.commit(seq);
          CHECK(queue.current(seq) == nullptr, "chunk %llu: still open after commit", (unsigned long long)seq);
        }
        auto &commit = queue.begin(seq);
        CHECK(queue.current(seq) == &commit, "chunk %llu: current() isn't the open commit", (unsigned long long)seq);
        encodeFill(queue, commit, buffer, seq, kSlots);
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
      // gw2 (#153): a chunk whose packets named nothing for its set, on the default path (the set
      // committed and used anyway) and with DXMT_MTL4_DEVICE_RESIDENCY=1 (committed without it)
      for (bool device_residency : {false, true}) {
        dxmt::gw2::g_device_residency = device_residency;
        uint64_t seq = kChunks + 1 + device_residency;
        queue.begin(seq);
        queue.commit(seq);
        auto &commits = queue.wait(seq);
        CHECK(commits.size() == 1 && commits[0]->done() && !commits[0]->feedback.error,
              "an empty chunk (device residency %d): %zu commits, or not done, or a GPU error", device_residency,
              commits.size());
        CHECK(queue.residency(seq).allocationCount() == 0, "an empty chunk's set has allocations");
        queue.retire(seq);
      }
      dxmt::gw2::g_device_residency = false;
      queue.waitIdle();
    }
    // gw2 (#204)
    allocatorPoolCase(mtl_device, device, true);
    allocatorPoolCase(mtl_device, device, false);
    std::printf("Mtl4Queue: %llu chunks on %u slots, split commits, a queue-level event wait: %s\n",
                (unsigned long long)kChunks, kSlots, failures ? "FAILED" : "ok");
  }
  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
