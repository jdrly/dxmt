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

#pragma once

#include "Metal.hpp"
#include "dxmt_allocation.hpp"
#include "dxmt_capture.hpp"
#include "dxmt_command.hpp"
#include "dxmt_command_list.hpp"
#include "dxmt_context.hpp"
#include "dxmt_counter.hpp"
#include "dxmt_gw2_lifetime.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_statistics.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_cpu_fence.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace dxmt {

template <typename T> class moveonly_list {
public:
  moveonly_list(T *storage, size_t size) : storage(storage), size_(size) {
    for (unsigned i = 0; i < size_; i++) {
      new (storage + i) T();
    }
  }

  moveonly_list(const moveonly_list &copy) = delete;
  moveonly_list(moveonly_list &&move) {
    storage = move.storage;
    move.storage = nullptr;
    size_ = move.size_;
    move.size_ = 0;
  };

  ~moveonly_list() {
    for (unsigned i = 0; i < size_; i++) {
      storage[i].~T();
    }
  };

  T &
  operator[](int index) {
    return storage[index];
  }

  std::span<T>
  span() const {
    return std::span<T>(storage, size_);
  }

  T *
  data() const {
    return storage;
  }

  size_t
  size() const {
    return size_;
  }

private:
  T *storage;
  size_t size_;
};

constexpr uint32_t kCommandChunkCount = 32;
// gw2 (#8): the chunk array's size; the ring itself is CommandQueue::ChunkCount() (DXMT_CHUNKS:
// 32, 64 or 128). Every chunks[seq % n] uses the runtime count.
constexpr uint32_t kCommandChunkCountMax = 128;

class CommandQueue;

class CommandChunk {
public:
  CommandChunk(const CommandChunk &) = delete; // delete copy constructor

  void *
  allocate_cpu_heap(size_t size, size_t alignment);

  template <CommandWithContext<ArgumentEncodingContext> F>
  void
  emitcc(F &&func) {
    list_enc.emit(std::forward<F>(func), allocate_cpu_heap(list_enc.calculateCommandSize<F>(), 16));
  }

  void
  encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc) {
    // flushCommands may commit cmdbuf and continue in a new one
    enc.$$setEncodingContext(
      chunk_id,
      frame_
    );
    auto& statistics = enc.currentFrameStatistics();
    auto t0 = clock::now();
    list_enc.execute(enc);
    auto t1 = clock::now();
    readback = enc.flushCommands(cmdbuf, chunk_id, chunk_event_id);
    attached_cmdbuf = cmdbuf; // the last command buffer of the chunk, the one to commit and wait for
    auto t2 = clock::now();
    statistics.encode_prepare_interval += (t1 - t0);
    statistics.encode_flush_interval += (t2 - t1);
  };

  uint64_t chunk_id;
  uint64_t chunk_event_id;
  uint64_t frame_;
  uint64_t signal_frame_latency_fence_;
  QueryReadbacks readback;
  uint64_t resource_initializer_event_id;

private:
  CommandQueue *queue;
  WMT::Reference<WMT::CommandBuffer> attached_cmdbuf;
  // gw2 (#3, #10): the buffers the present split committed before attached_cmdbuf (one per Present
  // in the chunk), kept until the chunk completes: the finish thread checks each for a GPU error
  // and, with DXMT_STATS on, reads its GPU timestamps. reset() keeps the capacity.
  std::vector<WMT::Reference<WMT::CommandBuffer>> split_cmdbufs;
  // gw2 (#31): what the chunk's command buffers use that nothing else holds until the chunk retires
  // (CommandQueue::KeepUntilRetired, with DXMT_UNRETAINED=1 or the check; empty otherwise): Metal
  // references (the drawable, events) owned here, and the presenter and MetalFX scalers. Released in
  // reset(), on the finish thread once the GPU is done.
  std::vector<obj_handle_t> keep_objects;
  std::vector<Rc<Presenter>> keep_presenters;
  std::vector<Rc<SpatialScaler>> keep_spatial_scalers;
  std::vector<Rc<TemporalScaler>> keep_temporal_scalers;
  
  CommandList<ArgumentEncodingContext> list_enc;
  AllocationRefTracking ref_tracker;

  friend class CommandQueue;

public:
  CommandChunk() {}

  void
  reset() noexcept {
    signal_frame_latency_fence_ = ~0ull;
    readback = {};
    list_enc.reset();
    ref_tracker.clear();
    attached_cmdbuf = nullptr;
    split_cmdbufs.clear();
    // gw2 (#31): after the command buffers, as Metal 3 releases what they retained
    for (auto object : keep_objects)
      WMT::Object{object}.release();
    keep_objects.clear();
    keep_presenters.clear();
    keep_spatial_scalers.clear();
    keep_temporal_scalers.clear();
  }
};

class CommandQueue {

private:
  void CommitChunkInternal(CommandChunk &chunk, uint64_t seq);

  uint32_t EncodingThread();

  uint32_t WaitForFinishThread();

  // gw2 (#10): a GPU error in a split-off buffer of chunk `seq`; finish thread only
  void ReportSplitError(CommandChunk &chunk, uint64_t seq, size_t index, WMT::CommandBuffer cmdbuf);
  uint64_t split_errors_ = 0;        // all of them, logged or not
  uint64_t split_errors_logged_ = 0; // split_errors_ at the last line
  clock::time_point split_error_last_line_{};

  std::atomic_uint64_t ready_for_encode = 1; // we start from 1, so 0 is always coherent
  std::atomic_uint64_t ready_for_commit = 1;
  std::atomic_uint64_t chunk_ongoing = 0;
  CpuFence cpu_coherent;
  CpuFence frame_latency_fence_;
  std::atomic_bool stopped;

  // gw2 (#8): the ring's size (DXMT_CHUNKS), the Metal queue's bound on command buffers, and
  // DXMT_MAX_FRAME_LATENCY (0: none). Declared before the threads and the Metal queue.
  const uint32_t chunk_count_;
  const uint32_t metal_buffer_count_;
  const uint32_t max_latency_override_;
  // gw2 (#31): DXMT_UNRETAINED, and this queue's id for DXMT_LIFETIME_CHECK's stamps
  const bool unretained_;
  const uint32_t lifetime_id_;

public:
  // gw2 (#8): one frame's RING totals, kept by CommitCurrentChunk until the next frame's first commit
  struct RingFrame {
    uint64_t frame = ~0ull;
    uint64_t commits = 0;
    uint64_t occupancy_sum = 0;
    uint64_t peak = 0;
    uint64_t stalls = 0;
    uint64_t stall_ns = 0;
  };

private:
  RingFrame ring_frame_;

  std::array<CommandChunk, kCommandChunkCountMax> chunks;
  uint64_t encoder_seq = 1;
  uint64_t frame_count = 0;
  uint32_t max_latency_ = max_latency_override_ ? max_latency_override_ : 3; // gw2 (#8)

  dxmt::thread encodeThread;
  dxmt::thread finishThread;
  WMT::Device device;
  WMT::Reference<WMT::CommandQueue> commandQueue;

  obj_handle_t shared_event_listener;
  dxmt::thread event_listener_thread;

  friend class CommandChunk;
  uint64_t
  GetNextEncoderId() {
    return encoder_seq++;
  }

  RingBumpState<StagingBufferBlockAllocator> staging_allocator;
  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_allocator;
  RingBumpState<StagingBufferBlockAllocator, kCommandChunkGPUHeapSize> argbuf_allocator;
  RingBumpState<HostBufferBlockAllocator, kCommandChunkCPUHeapSize, dxmt::null_mutex> cpu_command_allocator;
  RingBumpState<HostBufferBlockAllocator, 0x1000 /* 4kB */> reftracker_storage_allocator;
  CaptureState capture_state;

public:
  // a command buffer on this queue, for work split off a chunk (the present split)
  WMT::CommandBuffer
  CreateCommandBuffer() {
    // gw2 (#31): DXMT_UNRETAINED=1: one that doesn't retain what it uses (the chunk holds it)
    return unretained_ ? commandQueue.commandBufferWithUnretainedReferences() : commandQueue.commandBuffer();
  }

  // gw2 (#31): chunk `seq` (being encoded, encode thread only) holds these until it retires; called
  // only with gw2::lifetime::keepUntilRetired() (DXMT_UNRETAINED=1 or the check)
  template <typename T>
  void
  KeepUntilRetired(uint64_t seq, WMT::Reference<T> &&object) {
    if (object)
      chunks[seq % chunk_count_].keep_objects.push_back(std::exchange(object.handle, NULL_OBJECT_HANDLE));
  }
  void
  KeepUntilRetired(uint64_t seq, Rc<Presenter> &&presenter) {
    if (presenter)
      chunks[seq % chunk_count_].keep_presenters.push_back(std::move(presenter));
  }
  void
  KeepUntilRetired(uint64_t seq, Rc<SpatialScaler> &&scaler) {
    if (scaler)
      chunks[seq % chunk_count_].keep_spatial_scalers.push_back(std::move(scaler));
  }
  void
  KeepUntilRetired(uint64_t seq, Rc<TemporalScaler> &&scaler) {
    if (scaler)
      chunks[seq % chunk_count_].keep_temporal_scalers.push_back(std::move(scaler));
  }

  // gw2 (#31): DXMT_LIFETIME_CHECK's id of this queue
  uint32_t LifetimeId() const { return lifetime_id_; }

  // gw2 (#31): blocks until the finish thread has retired every chunk committed so far (device
  // teardown with DXMT_UNRETAINED=1 or DXMT_LIFETIME_CHECK=1, before the device's caches go)
  void
  WaitCommittedRetired() {
    auto committed = ready_for_encode.load(std::memory_order_acquire) - 1;
    if (committed)
      cpu_coherent.wait(committed);
  }

  // gw2 (#3, #10): a buffer the present split committed off chunk `seq`, for the finish thread's
  // error check and the GPU line
  void
  KeepSplitCommandBuffer(uint64_t seq, WMT::CommandBuffer cmdbuf) {
    chunks[seq % chunk_count_].split_cmdbufs.emplace_back(cmdbuf);
  }

  // gw2 (#3): this queue's completed frames' GPU spans (filled by its finish thread)
  gw2::GpuCollector gpu_collector;

  InternalCommandLibrary cmd_library;
  ArgumentEncodingContext argument_encoding_ctx;
  WMT::Reference<WMT::SharedEvent> event;
  std::uint64_t current_event_seq_id = 0;
  FrameStatisticsContainer statistics;
  // gw2 (#4): why the next CommitCurrentChunk happens, for CMDS' cbwhy. Set through
  // gw2::CommitWhyScope by the commit's caller (the immediate context, the swapchain); same thread
  // rules as CommitCurrentChunk.
  gw2::CommitWhy gw2_commit_why = gw2::CommitWhy::None;
  ResourceInitializer initializer;
  CounterPool counter_pool;

  CommandQueue(WMT::Device device);

  ~CommandQueue();

  CommandChunk *
  CurrentChunk() {
    auto id = ready_for_encode.load(std::memory_order_relaxed);
    return &chunks[id % chunk_count_];
  };

  uint64_t
  CoherentSeqId() {
    return cpu_coherent.signaledValue();
  };

  uint64_t
  CurrentSeqId() {
    return ready_for_encode.load(std::memory_order_relaxed);
  };

  uint64_t
  GetNextEventSeqId() {
    return ++current_event_seq_id;
  };

  uint64_t
  GetCurrentEventSeqId() {
    return current_event_seq_id;
  };


  uint64_t
  SignaledEventSeqId() {
    return event.signaledValue();
  };

  obj_handle_t GetSharedEventListener() {
    return shared_event_listener;
  }

  /**
  This is not thread-safe!
  CurrentChunk & CommitCurrentChunk should be called on the same thread

  */
  void CommitCurrentChunk();

  uint64_t CurrentFrameSeq() {
    return frame_count + 1;
  }

  FrameStatistics& CurrentFrameStatistics() {
    return statistics.at(frame_count);
  }

  void
  PresentBoundary() {
    statistics.compute(frame_count);
    frame_count++;
    statistics.at(frame_count).reset();
    // After present N-th frame (N starts from 1), wait for (N - max_latency)-th frame to finish rendering 
    if (likely(frame_count > max_latency_)) {
      auto t0 = clock::now();
      frame_latency_fence_.wait(frame_count - max_latency_);
      auto t1 = clock::now();
      statistics.at(frame_count).present_latency_interval += (t1 - t0);
    }
    statistics.at(frame_count).latency = max_latency_;
  }

  uint32_t GetMaxLatency() { return max_latency_; }

  void SetMaxLatency(uint32_t value) { max_latency_ = value; };

  // gw2 (#8): DXMT_MAX_FRAME_LATENCY, 0 when unset
  uint32_t MaxLatencyOverride() const { return max_latency_override_; }

  // gw2 (#8): the ring's chunk count in effect (DXMT_CHUNKS)
  uint32_t ChunkCount() const { return chunk_count_; }

  void
  WaitCPUFence(uint64_t seq) {
    cpu_coherent.wait(seq);
  };

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset, block.gpu_address};
  }

  std::tuple<void *, WMT::Buffer, uint64_t>
  AllocateArgumentBuffer(uint64_t seq, size_t size) {
    auto [block, offset] = argbuf_allocator.allocate(seq, cpu_coherent.signaledValue(), size, 64);
    return {ptr_add(block.mapped_address, offset), block.buffer, offset};
  }

  void *
  AllocateCommandData(size_t size, size_t alignment) {
    auto [block, offset] =
        cpu_command_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return ptr_add(block.ptr, offset);
  }

  void Retain(uint64_t seq, Allocation *allocation);
};

} // namespace dxmt