/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Marc-Aurel Zent for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxmt_command_queue.hpp"
#include "dxmt_gw2_compile.hpp"
#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_residency.hpp"
#include "dxmt_gw2_metal4.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include "Metal.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>
#include <utility>

#define ASYNC_ENCODING 1

namespace dxmt {

namespace {

// gw2 (#8): DXMT_CHUNKS, the ring's size: 32 (default, upstream's), 64 or 128 (strtoul's value;
// anything else is the default, with a warning). Read at each queue's creation, as before.
// gw2 (#60): declared here; the startup line shows the ring in effect (LogStartup's field).
sw::Switch<uint32_t> kChunks{"DXMT_CHUNKS", kCommandChunkCount, sw::legacy::ulongOneOf<32, 64, 128>};

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

gw2::SectionRegistrar g_ring_section("RING", RingSection);

} // namespace

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue->AllocateCommandData(size, alignment);
}

CommandQueue::CommandQueue(WMT::Device device) :
    chunk_count_(ChunkCountFromEnv()),
    metal_buffer_count_(MetalBufferCount(chunk_count_)),
    max_latency_override_(MaxLatencyOverrideFromEnv()),
    unretained_(gw2::lifetime::unretained()),
    lifetime_id_(gw2::lifetime::attachQueue()), // gw2 (#31): logs the command buffer mode once per queue
    encodeThread([this]() { this->EncodingThread(); }),
    finishThread([this]() { this->WaitForFinishThread(); }),
    device(device),
    commandQueue(device.newCommandQueue(metal_buffer_count_)),
    shared_event_listener(SharedEventListener_create()),
    event_listener_thread([this]() { SharedEventListener_start(this->shared_event_listener); }),
    staging_allocator({
        device, WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                    WMTResourceStorageModeManaged, false
    }),
    copy_temp_allocator({device, WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}),
    argbuf_allocator({
        device,
        WMTResourceHazardTrackingModeUntracked | WMTResourceCPUCacheModeWriteCombined | WMTResourceStorageModeShared
    }),
    cpu_command_allocator({}),
    reftracker_storage_allocator({}),
    cmd_library(device),
    argument_encoding_ctx(*this, device, cmd_library),
    initializer(device),
    counter_pool(device) {
  for (unsigned i = 0; i < kCommandChunkCountMax; i++) {
    auto &chunk = chunks[i];
    chunk.queue = this;
    chunk.reset();
  };
  event = device.newSharedEvent();
  // gw2 (#29): the device's residency set on this queue (DXMT_RESIDENCY_SET); logs once per queue
  gw2::residencyAttachQueue(device, commandQueue);
  // gw2 (#32): DXMT_METAL4 (on by default), decided once per process; with it =0 this does nothing
  gw2::metal4AttachQueue(device);
  if (gw2::metal4Active()) {
    auto mtl4 = std::make_unique<gw2::Mtl4Queue>(device, chunk_count_);
    if (!mtl4->valid())
      gw2::metal4Failed("creating the Metal 4 queue, its event or its allocators");
    else if (!gw2::residencyAttachMtl4Queue(device, mtl4->queue()))
      gw2::metal4Failed("attaching the residency set to the Metal 4 queue");
    else {
      mtl4_ = std::move(mtl4);
      Logger::info(str::format(
          "Metal 4: on (DXMT_METAL4=1, the default): this device's chunks go to an MTL4CommandQueue, with ", chunk_count_,
          " command allocators (one per chunk slot) and commit feedback"
      ));
    }
  }

  // gw2 (#8): the ring and the frame latency in effect, once per device
  g_ring_chunks.store(chunk_count_, std::memory_order_relaxed);
  Logger::info(str::format(
      "CommandQueue: chunks=", chunk_count_, kChunks.requested().empty() ? " (default)" : " (DXMT_CHUNKS)",
      " metalbuffers=", metal_buffer_count_, " maxlatency=", max_latency_,
      max_latency_override_ ? " (DXMT_MAX_FRAME_LATENCY, replaces the game's)" : " (until the game sets it)"
  ));

  std::string env = env::getEnvVar("DXMT_CAPTURE_FRAME");

  if (!env.empty()) {
    try {
      capture_state.scheduleNextFrameCapture(std::stoull(env));
    } catch (const std::invalid_argument &) {
    }
  }
}

CommandQueue::~CommandQueue() {
  TRACE("Destructing command queue");
  stopped.store(true);
  ready_for_encode++;
  ready_for_encode.notify_one();
  ready_for_commit++;
  ready_for_commit.notify_one();
  SharedEventListener_destroy(shared_event_listener);
  encodeThread.join();
  finishThread.join();
  // gw2 (#29): with the residency set, a chunk's references are what keep its allocations in the set
  // (and alive: the command buffers no longer retain them through useResource). The finish thread
  // stopped without retiring every committed chunk, so wait for their command buffers first; the
  // split-off ones were committed earlier on this queue and complete before their chunk's last.
  // gw2 (#31): the same with DXMT_UNRETAINED=1 (the command buffers retain nothing at all) and with
  // DXMT_LIFETIME_CHECK=1 (whose stamps assume it).
  // gw2 (#32): Metal 4: every committed command buffer (attached_cmdbuf is null for its chunks)
  if (mtl4_)
    mtl4_->waitIdle();
  if (gw2::residencyActive() || gw2::lifetime::keepUntilRetired()) {
    for (unsigned i = 0; i < kCommandChunkCountMax; i++) {
      auto &cmdbuf = chunks[i].attached_cmdbuf;
      if (!cmdbuf)
        continue;
      auto status = cmdbuf.status();
      if (status == WMTCommandBufferStatusCommitted || status == WMTCommandBufferStatusScheduled)
        cmdbuf.waitUntilCompleted();
    }
  }
  gw2::lifetime::detachQueue(lifetime_id_); // gw2 (#31): nothing of this queue is in use any more
  for (unsigned i = 0; i < kCommandChunkCountMax; i++) {
    auto &chunk = chunks[i];
    chunk.reset();
  };
  event_listener_thread.join();
  TRACE("Destructed command queue");
}

void
CommandQueue::CommitCurrentChunk() {
  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % chunk_count_];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.frame_ = frame_count;
  chunk.resource_initializer_event_id = initializer.flushToWait();
  auto& statistics = CurrentFrameStatistics();
  statistics.command_buffer_count++;
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();

  auto t0 = clock::now();
  // gw2 (#8): at most chunk_count_ - 1 chunks in flight; a full ring blocks here (a ring stall)
  const uint64_t ring_limit = chunk_count_ - 1;
  bool ring_full = chunk_ongoing.load(std::memory_order_acquire) >= ring_limit;
  if (ring_full)
    chunk_ongoing.wait(ring_limit, std::memory_order_acquire);
  uint64_t in_flight = chunk_ongoing.fetch_add(1, std::memory_order_relaxed) + 1;
  auto t1 = clock::now();
  statistics.commit_interval += (t1 - t0);
  if (gw2::statsEnabled())
    gw2::waitAdd(gw2::Wait::Commit, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

  if (gw2::statsEnabled()) {
    // gw2 (#8): the first commit of a new frame publishes the previous frame's totals
    if (ring_frame_.frame != frame_count) {
      if (ring_frame_.commits)
        PublishRingFrame(ring_frame_);
      ring_frame_ = {.frame = frame_count};
    }
    ring_frame_.commits++;
    ring_frame_.occupancy_sum += in_flight;
    ring_frame_.peak = std::max(ring_frame_.peak, in_flight);
    if (ring_full) {
      ring_frame_.stalls++;
      ring_frame_.stall_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
  }

#else
  CommitChunkInternal(chunk, ready_for_encode.fetch_add(1, std::memory_order_relaxed));
#endif

  cpu_command_allocator.free_blocks(cpu_coherent.signaledValue());
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk, uint64_t seq) {

  auto pool = WMT::MakeAutoreleasePool();

  switch (capture_state.getNextAction(chunk.frame_)) {
  case CaptureState::NextAction::StartCapture: {
    WMTCaptureInfo info;
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    info.capture_object = device;
    info.destination = WMTCaptureDestinationGPUTraceDocument;
    char filename[1024];
    std::time_t now;
    std::time(&now);
    std::strftime(filename, 1024, "_%H'%M'%S_%m-%d-%y.gputrace", std::localtime(&now));
    auto fileUrl = env::getUnixPath(env::getExeBaseName() + "_F." + std::to_string(chunk.frame_) + filename);
    WARN("A new capture will be saved to ", fileUrl);
    info.output_url.set(fileUrl.c_str());

    capture_mgr.startCapture(info);
    break;
  }
  case CaptureState::NextAction::StopCapture: {
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    capture_mgr.stopCapture();
    break;
  }
  case CaptureState::NextAction::Nothing: {
    if (capture_state.shouldCaptureNextFrame()) {
      capture_state.scheduleNextFrameCapture(chunk.frame_ + 1);
    }
    break;
  }
  }

  // gw2 (#5): close the previous frame's GetPipeline waits (COMPILE frame= lines)
  gw2::compileEncodeFrame(chunk.frame_);

  if (mtl4_) { // gw2 (#32): Metal 4
    CommitChunkMetal4(chunk, seq);
    ready_for_commit.fetch_add(1, std::memory_order_release);
    ready_for_commit.notify_one();
    return;
  }

  auto cmdbuf = CreateCommandBuffer(); // gw2 (#31): unretained with DXMT_UNRETAINED=1
  chunk.attached_cmdbuf = cmdbuf;
  if (chunk.resource_initializer_event_id) {
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
    gw2::lifetime::use(lifetime_id_, seq, initializer.event(), gw2::lifetime::Kind::Event); // gw2 (#31)
  }
  chunk.encode(chunk.attached_cmdbuf, this->argument_encoding_ctx);
  gw2::residencyBeforeCommit(); // gw2 (#29): the set's pending adds and removes, if any
  chunk.attached_cmdbuf.commit();

  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_one();
}

uint32_t
CommandQueue::EncodingThread() {
#if ASYNC_ENCODING
  env::setThreadName("dxmt-encode-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_encode.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    // perform...
    auto &chunk = chunks[internal_seq % chunk_count_];
    CommitChunkInternal(chunk, internal_seq);
    internal_seq++;
  }
  TRACE("encoder thread gracefully terminates");
#endif
  return 0;
}

uint32_t
CommandQueue::WaitForFinishThread() {
  env::setThreadName("dxmt-finish-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_commit.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    auto &chunk = chunks[internal_seq % chunk_count_];
    if (mtl4_) { // gw2 (#32): Metal 4: the chunk's commits, from their feedback
      FinishChunkMetal4(chunk, internal_seq);
    } else {
      if (chunk.attached_cmdbuf.status() <= WMTCommandBufferStatusScheduled) {
        chunk.attached_cmdbuf.waitUntilCompleted();
      }
      if (chunk.attached_cmdbuf.status() == WMTCommandBufferStatusError) {
        ERR("Device error at frame ", chunk.frame_, ": ", chunk.attached_cmdbuf.error().description().getUTF8String());
      }
      if (auto logs = chunk.attached_cmdbuf.logs()) {
        for (auto &log : logs.elements()) {
          ERR("Frame ", chunk.frame_, ": ", log.description().getUTF8String());
        }
      }

      // gw2 (#10): the buffers the present split committed first (earlier on this queue, so done or
      // about to be), checked for a GPU error as the last one is. The status is the read the GPU line
      // needed already; a buffer still pending is waited for here, never on the encode thread.
      // gw2 (#3): their GPU execution spans, then the last buffer's, for the GPU line
      bool stats = gw2::statsEnabled();
      for (size_t i = 0; i < chunk.split_cmdbufs.size(); i++) {
        auto &split = chunk.split_cmdbufs[i];
        auto status = split.status();
        if (status <= WMTCommandBufferStatusScheduled) {
          split.waitUntilCompleted();
          status = split.status();
        }
        if (unlikely(status == WMTCommandBufferStatusError))
          ReportSplitError(chunk, internal_seq, i, split);
        if (stats)
          gpu_collector.add(chunk.frame_, split.gpuStartTime(), split.gpuEndTime());
      }
      if (stats) {
        uint64_t gpu_end = chunk.attached_cmdbuf.gpuEndTime();
        gpu_collector.add(chunk.frame_, chunk.attached_cmdbuf.gpuStartTime(), gpu_end);
        gw2::presentGpuEnd(chunk.frame_, gpu_end); // gw2 (#7): PRESENT's aftergpu
      }
    }

    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    // gw2 (#31): the GPU is done with every buffer of the chunk; from here its references may go
    gw2::lifetime::completed(lifetime_id_, internal_seq);
    chunk.reset();
    // gw2 (#32): after reset (its readbacks read the commits' feedback); resets the slot's allocator
    if (mtl4_)
      mtl4_->retire(internal_seq);
    cpu_coherent.signal(internal_seq);
    chunk_ongoing.fetch_sub(1, std::memory_order_release);
    chunk_ongoing.notify_one();

    staging_allocator.free_blocks(internal_seq);
    copy_temp_allocator.free_blocks(internal_seq);
    argbuf_allocator.free_blocks(internal_seq);

    internal_seq++;
  }
  TRACE("finishing thread gracefully terminates");
  return 0;
}

// gw2 (#10): the line for a GPU error in a split-off buffer, with the chunk's frame and sequence id,
// then the buffer's logs, as for the chunk's last buffer. A GPU fault tends to repeat every frame,
// so the first 8 are logged, then at most one line a second, each with the count of those not logged.
bool
CommandQueue::SplitErrorLine(uint64_t &not_logged) {
  split_errors_++;
  auto now = clock::now();
  if (split_errors_ > 8 && now - split_error_last_line_ < std::chrono::seconds(1))
    return false;
  not_logged = split_errors_ - split_errors_logged_ - 1;
  split_errors_logged_ = split_errors_;
  split_error_last_line_ = now;
  return true;
}

void
CommandQueue::ReportSplitError(CommandChunk &chunk, uint64_t seq, size_t index, WMT::CommandBuffer cmdbuf) {
  uint64_t not_logged;
  if (!SplitErrorLine(not_logged))
    return;
  ERR("Device error at frame ", chunk.frame_, " (chunk ", seq, ", split-off buffer ", index + 1, " of ",
      chunk.split_cmdbufs.size(), ", split error ", split_errors_, ", ", not_logged, " not logged since the last line): ",
      cmdbuf.error().description().getUTF8String());
  if (auto logs = cmdbuf.logs()) {
    for (auto &log : logs.elements()) {
      ERR("Frame ", chunk.frame_, " (chunk ", seq, ", split-off buffer ", index + 1, "): ",
          log.description().getUTF8String());
    }
  }
}

// gw2 (#32): Metal 4 has no command buffer logs; the error is the commit feedback's
void
CommandQueue::ReportSplitError(CommandChunk &chunk, uint64_t seq, size_t index, size_t count, WMT::Error error) {
  uint64_t not_logged;
  if (!SplitErrorLine(not_logged))
    return;
  ERR("Device error at frame ", chunk.frame_, " (chunk ", seq, ", split-off buffer ", index + 1, " of ", count,
      ", split error ", split_errors_, ", ", not_logged, " not logged since the last line): ",
      error.description().getUTF8String());
}

// gw2 (#32): the chunk's Metal 4 command buffers: a wait for the initializer's uploads at the queue,
// then the chunk's encoders (gw2::Mtl4Encoding: command buffers begin as encoders need them; queue
// events, the present split and the present pass commit; each commit carries the residency sets'
// pending changes and its feedback), then the rest left open.
void
CommandQueue::CommitChunkMetal4(CommandChunk &chunk, uint64_t seq) {
  if (chunk.resource_initializer_event_id) {
    mtl4_->waitForEvent(initializer.event(), chunk.resource_initializer_event_id);
    gw2::lifetime::use(lifetime_id_, seq, initializer.event(), gw2::lifetime::Kind::Event); // gw2 (#31)
  }
  // the encoders (command buffers begin as they need them; events and the present split commit)
  gw2::Mtl4Encoding encoding(*mtl4_, seq, [this, seq](uint64_t size) {
    auto [mapped, buffer, offset] = AllocateArgumentBuffer(seq, size);
    return std::pair<WMT::Buffer, uint64_t>{buffer, offset};
  });
  chunk.encodeMetal4(encoding, argument_encoding_ctx);
  encoding.commitOpen(); // the rest, if anything is left open
}

// gw2 (#32): the finish thread's part for a Metal 4 chunk: waits for every commit's feedback, then
// does what the Metal 3 path does with the command buffers: device errors (the last buffer's as
// upstream's line, earlier ones as split errors), GPU spans for the GPU line, PRESENT's aftergpu.
void
CommandQueue::FinishChunkMetal4(CommandChunk &chunk, uint64_t seq) {
  auto &commits = mtl4_->wait(seq);
  // The chunk's last queue operation is the signal of its completion event (flushCommands' end,
  // Mtl4Encoding::encodeSignalEvent), ordered after every commit and every queue-level wait of the
  // chunk. A chunk with no command buffer, or with queue waits or signals after its last one, is
  // complete only once it fired: wait for it too before anything of the chunk is released.
  if (event.signaledValue() < chunk.chunk_event_id) {
    bool warned = false;
    while (!event.waitUntilSignaledValue(chunk.chunk_event_id, 1000)) {
      if (!warned)
        WARN("Metal 4: chunk ", seq, "'s completion event not signalled after 1 s; still waiting");
      warned = true;
    }
  }
  bool stats = gw2::statsEnabled();
  size_t splits = commits.empty() ? 0 : commits.size() - 1;
  for (size_t i = 0; i < commits.size(); i++) {
    auto *commit = commits[i];
    if (unlikely(commit->feedback.error != NULL_OBJECT_HANDLE)) {
      if (i < splits)
        ReportSplitError(chunk, seq, i, splits, commit->error());
      else
        ERR("Device error at frame ", chunk.frame_, ": ", commit->error().description().getUTF8String());
    }
    if (stats)
      gpu_collector.add(chunk.frame_, commit->gpuStartNs(), commit->gpuEndNs());
  }
  if (stats && !commits.empty())
    gw2::presentGpuEnd(chunk.frame_, commits.back()->gpuEndNs()); // gw2 (#7)
}

void CommandQueue::Retain(uint64_t seq, Allocation* allocation) {
  auto &chunk = chunks[seq % chunk_count_];
  auto &tracker = chunk.ref_tracker;
  gw2::lifetime::useAllocation(lifetime_id_, seq, allocation); // gw2 (#31)
  constexpr size_t block_size = decltype(reftracker_storage_allocator)::block_size;
  while (unlikely(!tracker.track(allocation))) {
    auto [temp_buffer, _] = reftracker_storage_allocator.allocate(seq, cpu_coherent.signaledValue(), block_size, 1);
    tracker.addStorage(temp_buffer.ptr, block_size);
  }
};

} // namespace dxmt
