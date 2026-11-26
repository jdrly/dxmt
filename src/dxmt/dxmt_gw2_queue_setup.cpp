// gw2 (#312): see dxmt_gw2_queue_setup.hpp. The switches, the RING section and the body of
// gw2AttachSubmission are dxmt_command_queue.cpp's code, moved here unchanged.
#include "dxmt_gw2_queue_setup.hpp"
#include "dxmt_gw2_deps.hpp"
#include "dxmt_gw2_lifetime.hpp"
#include "dxmt_gw2_metal4.hpp"
#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_residency.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_submission.hpp"
#include "dxmt_gw2_switches.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace dxmt {

namespace {

// gw2 (#8): DXMT_CHUNKS, the ring's size: 32 (default, upstream's), 64 or 128 (strtoul's value;
// anything else is the default, with a warning). Read at each queue's creation, as before.
// gw2 (#60): declared here; the startup line shows the ring in effect (LogStartup's field).
sw::Switch<uint32_t> kChunks{"DXMT_CHUNKS", kCommandChunkCount, sw::legacy::ulongOneOf<32, 64, 128>};

// gw2 (#113): DXMT_RETIRE_RECOVERY (on; 0: off): a refused retirement (never expected) recovers by
// sequence with the completion its token proves, and refused holds go once their last chunk completed,
// instead of every later hold being kept until teardown. Read at each queue's creation.
sw::Switch<bool> kRetireRecovery{"DXMT_RETIRE_RECOVERY", true, sw::legacy::notZero};

// gw2 (#269): DXMT_RELEASE_POOL (on; 0: off): the finish thread drains an autorelease pool around each
// chunk's retirement, and the bridge's unix side (_NSObject_release) one around each release that frees
// its object, which reads the switch itself, once, from the process environment (as DXMT_CACHE_DIR).
// Read here at each queue's creation.
sw::Switch<bool> kReleasePool{"DXMT_RELEASE_POOL", true, sw::legacy::notZero};

// gw2 (#316): DXMT_DRAIN_SPLITS, =1 only: Metal 3's teardown drain also waits for the split-off
// command buffers the ledger still holds. Read at each queue's creation.
sw::Switch<bool> kDrainSplits{"DXMT_DRAIN_SPLITS", false, sw::legacy::exactlyOne};

uint32_t
ChunkCountFromEnv() {
  auto v = kChunks.requested();
  if (!v.empty() && !sw::legacy::ulongIsOneOf<32, 64, 128>(v))
    WARN("DXMT_CHUNKS=", v, " is not 32, 64 or 128; using ", kCommandChunkCount);
  return kChunks.parse(v);
}

// gw2 (#8): the Metal queue's bound on uncompleted command buffers. Unset DXMT_CHUNKS keeps
// upstream's 32 (the ring's size). Set, it is the ring plus headroom for the present split's extra
// buffer: one per frame in flight, and SetMaximumFrameLatency allows at most 16 frames.
uint32_t
MetalBufferCount(uint32_t chunks) {
  if (kChunks.requested().empty())
    return kCommandChunkCount;
  return chunks + 16;
}

// gw2 (#8): DXMT_MAX_FRAME_LATENCY, 1-16, replaces the latency the game sets; 0: not set.
uint32_t
MaxLatencyOverrideFromEnv() {
  auto v = env::getEnvVar("DXMT_MAX_FRAME_LATENCY");
  if (v.empty())
    return 0;
  auto n = std::strtoul(v.c_str(), nullptr, 10);
  if (n >= 1 && n <= 16)
    return n;
  WARN("DXMT_MAX_FRAME_LATENCY=", v, " is not 1-16; ignored");
  return 0;
}

// gw2 (#151): DXMT_THREAD_PRIORITY, the encode and finish threads' priority (critical, normal or
// utility; see dxmt_gw2_thread_priority.hpp). Read at each queue's creation.
gw2::ThreadPrioritySwitch kThreadPriority{"DXMT_THREAD_PRIORITY"};

// gw2 (#8): the RING section. The render thread publishes each completed frame's totals (at the
// next frame's first commit) under the mutex, and the 2 s window drains them under it, so a line
// only ever holds whole frames and its averages divide by the frames it holds.
struct RingWindow {
  uint64_t frames = 0;
  uint64_t commits = 0;
  uint64_t occupancy_sum = 0; // chunks in flight after each commit
  uint64_t peak = 0;
  uint64_t stalls = 0;
  uint64_t stalled_frames = 0;
  uint64_t stall_ns = 0;
  uint64_t frame_stall_max_ns = 0;
};
std::mutex g_ring_mutex;
RingWindow g_ring_window;
std::atomic<uint32_t> g_ring_chunks{kCommandChunkCount};

void
PublishRingFrame(const CommandQueue::RingFrame &f) {
  std::lock_guard lock(g_ring_mutex);
  auto &w = g_ring_window;
  w.frames++;
  w.commits += f.commits;
  w.occupancy_sum += f.occupancy_sum;
  w.peak = std::max(w.peak, f.peak);
  w.stalls += f.stalls;
  w.stalled_frames += f.stalls ? 1 : 0;
  w.stall_ns += f.stall_ns;
  w.frame_stall_max_ns = std::max(w.frame_stall_max_ns, f.stall_ns);
}

std::string
RingSection(double, uint32_t) {
  RingWindow w;
  {
    std::lock_guard lock(g_ring_mutex);
    w = std::exchange(g_ring_window, {});
  }
  if (!w.commits)
    return {};
  double n = w.frames;
  return std::format(
      "chunks={} occupancy={:.1f}/{} stalls={:.2f} stalled={} ms={:.2f}/{:.1f}", g_ring_chunks.load(),
      double(w.occupancy_sum) / w.commits, w.peak, w.stalls / n, w.stalled_frames, w.stall_ns / 1e6 / n,
      w.frame_stall_max_ns / 1e6
  );
}

} // namespace

namespace gw2 {

uint32_t
queueChunkCount() {
  return ChunkCountFromEnv();
}

uint32_t
queueMetalBufferCount(uint32_t chunks) {
  return MetalBufferCount(chunks);
}

uint32_t
queueMaxLatencyOverride() {
  return MaxLatencyOverrideFromEnv();
}

bool
queueRetireRecovery() {
  return kRetireRecovery();
}

bool
queueReleasePool() {
  return kReleasePool();
}

ThreadPrioritySwitch &
queueThreadPriority() {
  return kThreadPriority;
}

void
publishRingFrame(const CommandQueue::RingFrame &frame) {
  PublishRingFrame(frame);
}

std::string
ringSection(double window_ms, uint32_t frames) {
  return RingSection(window_ms, frames);
}

} // namespace gw2

void
CommandQueue::gw2AttachSubmission(WMT::Device device) {
  // gw2 (#29): the device's residency set on this queue (DXMT_RESIDENCY_SET); logs once per queue
  gw2::residencyAttachQueue(device, commandQueue);
  // gw2 (#32): DXMT_METAL4 (on by default), decided once per process; with it =0 this does nothing
  gw2::metal4AttachQueue(device);
  if (gw2::metal4Active()) {
    // gw2 (#204): DXMT_MTL4_ALLOCATOR_POOL, read once per queue
    const bool allocator_pool = gw2::mtl4AllocatorPool();
    auto mtl4 = std::make_unique<gw2::Mtl4Queue>(device, chunk_count_, allocator_pool);
    if (!mtl4->valid())
      gw2::metal4Failed("creating the Metal 4 queue, its event or its allocators");
    else if (gw2::mtl4PresentMetal3() && !mtl4->enableMetal3Present())
      gw2::metal4Failed("creating the two events of DXMT_MTL4_PRESENT=mtl3");
    else if (!gw2::residencyAttachMtl4Queue(device, mtl4->queue()))
      gw2::metal4Failed("attaching the residency set to the Metal 4 queue");
    else {
      submission_ = std::make_unique<Metal4Submission>(std::move(mtl4), event); // gw2 (#78)
      metal4_ = true;
      // gw2 (#204): the allocator policy in effect
      if (allocator_pool)
        Logger::info(str::format(
            "Metal 4: on (DXMT_METAL4=1, the default): this device's chunks go to an MTL4CommandQueue, with command "
            "allocators from a pool (made when none is idle, at most ", gw2::Mtl4Queue::kIdleAllocators,
            " kept idle; DXMT_MTL4_ALLOCATOR_POOL=1) and commit feedback"
        ));
      else
        Logger::info(str::format(
            "Metal 4: on (DXMT_METAL4=1, the default): this device's chunks go to an MTL4CommandQueue, with ",
            chunk_count_, " command allocators (one per chunk slot; DXMT_MTL4_ALLOCATOR_POOL=0) and commit feedback"
        ));
      // gw2 (#184): the queue holds only the device's set; a queue takes at most 32
      Logger::info("Metal 4: drawables stay resident through their layer's residency set, used by each present's "
                   "command buffer; the queue holds no layer's set");
      // gw2 (#154)
      Logger::info(
          gw2::mtl4TimestampReuse()
              ? "Metal 4: timestamp queries read the chunk's last command buffer, the present's after a present "
                "(DXMT_MTL4_TIMESTAMP_REUSE=1)"
              : "Metal 4: a chunk with timestamp queries commits an empty command buffer for them after its last one "
                "(DXMT_MTL4_TIMESTAMP_REUSE=0)"
      );
      // gw2 (#227): the app sets it on 0.2 (ADR 0008); unset, no line
      if (gw2::mtl4PresentMetal3())
        Logger::info("Metal 4: present through a Metal 3 command buffer (DXMT_MTL4_PRESENT=mtl3)");
      // gw2 (#271): on by default with mtl3 (#332); DXMT_MTL4_SHOWN_WAIT=next, no line
      if (gw2::mtl4ShownWaitWrite()) {
        gw2::metal4ShownWaitInUse();
        Logger::info("Metal 4: the next frame waits for a Metal 3 present only before it writes the presented "
                     "texture (DXMT_MTL4_SHOWN_WAIT=write)");
      }
      // gw2 (#353): off by default (ADR 0018); unset, no line
      if (gw2::mtl4PresentMetal3() && gw2::mtl4PresentFences())
        Logger::info("Metal 4: a present's fence waits and update go to the Metal 4 queue, in an encoder of their "
                     "own before the frame's commit (DXMT_MTL4_PRESENT_FENCES=1)");
    }
  }
  // gw2 (#78): Metal 3, unless the queue is on Metal 4
  if (!submission_)
    submission_ = std::make_unique<Metal3Submission>(
        chunk_count_,
        Metal3Submission::Hooks{
            .new_command_buffer = [this] { return CreateCommandBuffer(); },
            .before_commit = gw2::residencyBeforeCommit,
            .split_buffers = [this](uint64_t seq) { return ledger_.splits(seq); }, // gw2 (#66)
            .wait_at_drain = [] { return gw2::residencyActive() || gw2::lifetime::keepUntilRetired(); },
            // gw2 (#316)
            .held_splits = [this, on = kDrainSplits()] {
              std::vector<WMT::CommandBuffer> out;
              if (on)
                ledger_.forEachHeldSplit([&](auto &cmdbuf) { out.push_back(WMT::CommandBuffer(cmdbuf)); });
              return out;
            },
        }
    );
  // gw2 (#69): which holds the chunks need, now that the queue is on Metal 3 or Metal 4 (before its
  // first chunk; the encode thread reads it only after a commit's release)
  ledger_.setPolicy({.metal4 = metal4_, .unretained = unretained_, .lifetime_check = gw2::lifetime::check()});
  // gw2 (#91): only this submission's tokens retire and drain the chunks' holds
  ledger_.acceptTokensFrom(submission_.get());
  // gw2 (#132): the encoder dependency policy (Metal 4 only), before the context's first encoder
  argument_encoding_ctx.setDependencyPolicy(
      gw2::dependencyCompressionAttach(metal4_), gw2::statsEnabled()
  );

  // gw2 (#8): the ring and the frame latency in effect, once per device
  g_ring_chunks.store(chunk_count_, std::memory_order_relaxed);
  Logger::info(str::format(
      "CommandQueue: chunks=", chunk_count_, kChunks.requested().empty() ? " (default)" : " (DXMT_CHUNKS)",
      " metalbuffers=", metal_buffer_count_, " maxlatency=", max_latency_,
      max_latency_override_ ? " (DXMT_MAX_FRAME_LATENCY, replaces the game's)" : " (until the game sets it)"
  ));
}

} // namespace dxmt
