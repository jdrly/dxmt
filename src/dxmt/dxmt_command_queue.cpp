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
#include "dxmt_gw2_stats.hpp"
#include "Metal.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <atomic>
#include <cstdlib>
#include <format>

#define ASYNC_ENCODING 1

namespace dxmt {

namespace {

// gw2 (#8): DXMT_CHUNKS, the ring's size: 32 (default, upstream's), 64 or 128.
uint32_t
ChunkCountFromEnv() {
  auto v = env::getEnvVar("DXMT_CHUNKS");
  if (v.empty())
    return kCommandChunkCount;
  auto n = std::strtoul(v.c_str(), nullptr, 10);
  if (n == 32 || n == 64 || n == 128)
    return n;
  WARN("DXMT_CHUNKS=", v, " is not 32, 64 or 128; using ", kCommandChunkCount);
  return kCommandChunkCount;
}

// gw2 (#8): the Metal queue's bound on uncompleted command buffers. Unset DXMT_CHUNKS keeps
// upstream's 32 (the ring's size). Set, it is the ring plus headroom for the present split's extra
// buffer: one per frame in flight, and SetMaximumFrameLatency allows at most 16 frames.
uint32_t
MetalBufferCount(uint32_t chunks) {
  if (env::getEnvVar("DXMT_CHUNKS").empty())
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

// gw2 (#8): the RING section. The render thread adds, the 2 s window drains.
struct RingStats {
  std::atomic<uint32_t> chunks{kCommandChunkCount};
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> occupancy_sum{0}; // chunks in flight after each commit
  std::atomic<uint64_t> peak{0};
  std::atomic<uint64_t> stalls{0};
  std::atomic<uint64_t> stalled_frames{0};
  std::atomic<uint64_t> stall_ns{0};
  std::atomic<uint64_t> frame_stall_max_ns{0};
};
RingStats g_ring;

void
atomicMax(std::atomic<uint64_t> &a, uint64_t v) {
  auto cur = a.load(std::memory_order_relaxed);
  while (cur < v && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed))
    ;
}

std::string
RingSection(double, uint32_t frames) {
  auto commits = g_ring.commits.exchange(0, std::memory_order_relaxed);
  auto occupancy = g_ring.occupancy_sum.exchange(0, std::memory_order_relaxed);
  auto peak = g_ring.peak.exchange(0, std::memory_order_relaxed);
  auto stalls = g_ring.stalls.exchange(0, std::memory_order_relaxed);
  auto stalled = g_ring.stalled_frames.exchange(0, std::memory_order_relaxed);
  auto stall_ns = g_ring.stall_ns.exchange(0, std::memory_order_relaxed);
  auto frame_max_ns = g_ring.frame_stall_max_ns.exchange(0, std::memory_order_relaxed);
  if (!commits)
    return {};
  double n = frames ? frames : 1;
  return std::format(
      "chunks={} occupancy={:.1f}/{} stalls={:.2f} stalled={} ms={:.2f}/{:.1f}", g_ring.chunks.load(),
      double(occupancy) / commits, peak, stalls / n, stalled, stall_ns / 1e6 / n, frame_max_ns / 1e6
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

  // gw2 (#8): the ring and the frame latency in effect, once per device
  g_ring.chunks.store(chunk_count_, std::memory_order_relaxed);
  Logger::info(str::format(
      "CommandQueue: chunks=", chunk_count_, env::getEnvVar("DXMT_CHUNKS").empty() ? " (default)" : " (DXMT_CHUNKS)",
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

  if (gw2::statsEnabled()) {
    g_ring.commits.fetch_add(1, std::memory_order_relaxed);
    g_ring.occupancy_sum.fetch_add(in_flight, std::memory_order_relaxed);
    atomicMax(g_ring.peak, in_flight);
    if (ring_full) {
      uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      if (ring_stall_frame_ != frame_count) {
        ring_stall_frame_ = frame_count;
        ring_frame_stall_ns_ = 0;
        g_ring.stalled_frames.fetch_add(1, std::memory_order_relaxed);
      }
      ring_frame_stall_ns_ += ns;
      g_ring.stalls.fetch_add(1, std::memory_order_relaxed);
      g_ring.stall_ns.fetch_add(ns, std::memory_order_relaxed);
      atomicMax(g_ring.frame_stall_max_ns, ring_frame_stall_ns_);
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

  auto cmdbuf = commandQueue.commandBuffer();
  chunk.attached_cmdbuf = cmdbuf;
  if (chunk.resource_initializer_event_id) {
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
  }
  chunk.encode(chunk.attached_cmdbuf, this->argument_encoding_ctx);
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

    // gw2: the chunk's GPU execution span for STATS' gpuspan/gpugap (#3)
    if (gw2::statsEnabled())
      gw2::gpuSpanAdd(chunk.frame_, chunk.attached_cmdbuf.gpuStartTime(), chunk.attached_cmdbuf.gpuEndTime());

    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    chunk.reset();
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

void CommandQueue::Retain(uint64_t seq, Allocation* allocation) {
  auto &chunk = chunks[seq % chunk_count_];
  auto &tracker = chunk.ref_tracker;
  constexpr size_t block_size = decltype(reftracker_storage_allocator)::block_size;
  while (unlikely(!tracker.track(allocation))) {
    auto [temp_buffer, _] = reftracker_storage_allocator.allocate(seq, cpu_coherent.signaledValue(), block_size, 1);
    tracker.addStorage(temp_buffer.ptr, block_size);
  }
};

} // namespace dxmt
