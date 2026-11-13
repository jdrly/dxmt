#pragma once

// gw2 (#32): Metal 4 submission for a DXMT command queue (DXMT_METAL4=1, jdrly/dxmt #32 part 2).
//
// - One MTL4CommandQueue, with the device's residency set attached (Metal 4 has no useResource).
// - One MTL4CommandAllocator per chunk slot (the ring's size, DXMT_CHUNKS): a chunk's command buffers
//   take their memory from their slot's allocator, which the finish thread resets when it retires
//   the chunk, after the GPU completed every buffer of it. gw2 (#204), with
//   DXMT_MTL4_ALLOCATOR_POOL=1: a slot has no allocator of its own; the chunk takes one from the
//   queue's pool at its first begin() and gives it back in retire(), after the reset. The pool makes
//   an allocator only when it has none idle and keeps at most kIdleAllocators idle (one more is
//   released).
// - A chunk commits one command buffer, or several (the present split commits the frame before
//   nextDrawable and the present pass gets its own, and queue-level events commit as well). Each commit carries its own
//   MTL4CommitOptions feedback (GPU start and end, error), written into an Mtl4Commit record, and
//   raises the queue's shared event to the commit's value once written. The finish thread waits on
//   that event instead of waitUntilCompleted.
// - Each slot also has a residency set of its own (part 3): the replays put every allocation the
//   chunk's packets name directly into it (Metal 4 makes nothing resident by itself); each commit
//   commits it and the command buffer uses it; retire() empties it. gw2 (#153), with
//   DXMT_MTL4_DEVICE_RESIDENCY=1: what the device's set holds stays out of it, and it is committed,
//   used and emptied only while it has allocations.
// - Command buffers and records are recycled (an MTL4CommandBuffer is reusable after completion).
// - gw2 (#227), DXMT_MTL4_PRESENT=mtl3: the present goes through a Metal 3 command buffer of the
//   command queue's MTLCommandQueue (Mtl4Encoding::beginMetal3Present). Two shared events order the
//   queues: this queue signals `frame` after the frame's commits, the Metal 3 buffer waits for it and
//   signals `shown` after the present pass, and this queue waits for `shown` before its next commit.
//   The Metal 3 buffer belongs to its chunk's slot: wait() waits for it, retire() lets it go.
// - gw2 (#242), DXMT_MTL4_PRESENT=done: the present pass's commit carries the drawable's signal and
//   present (commitPresent); the unix side presents once the commit has completed. wait() reads when.
//
// Threads: begin/commit/waitForEvent/signalEvent on the encode thread, for the chunk being encoded;
// wait/retire on the finish thread, for the oldest chunk not retired; waitIdle at teardown, once
// both have stopped. The queue's existing chunk handshake (ready_for_commit, chunk_ongoing) orders
// a slot's use between them.
//
// The METAL4 section (2 s, with DXMT_STATS): commits= buffers=<per chunk, max> allocmb=<largest
// allocator at reset> allocators=<alive now> allocheld=<their sizes at their last reset, MB>
// slow=<chunks the finish thread waited for over 1 s> errors=. gw2 (#242): beforepass=<presents called
// before their pass's commit had completed>.

#include "Metal.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dxmt::gw2 {

// One Metal 4 command buffer of a chunk and what its commit's feedback handler writes. Its address
// is stable from begin() until retire() of its chunk.
struct Mtl4Commit {
  WMT::Reference<WMT::MTL4CommandBuffer> cmdbuf;
  WMTMtl4CommitFeedback feedback{};
  uint64_t value = 0; // the queue event's value once the feedback is written; 0 = not committed

  bool
  done() const {
    return __atomic_load_n(&feedback.done, __ATOMIC_ACQUIRE) != 0;
  }

  // after done(): the GPU times in ns (CACurrentMediaTime's clock, as Metal 3's gpuStartTime), 0 if
  // Metal gave none
  uint64_t
  gpuStartNs() const {
    return feedback.gpu_start > 0 ? uint64_t(feedback.gpu_start * 1e9) : 0;
  }
  uint64_t
  gpuEndNs() const {
    return feedback.gpu_end > 0 ? uint64_t(feedback.gpu_end * 1e9) : 0;
  }
  // after done(): the commit's error, or null
  WMT::Error
  error() const {
    return WMT::Error{feedback.error};
  }
};

// The argument tables a queue's Metal 4 encoders bind, one per stage (part 3). Created with the
// queue and reused by every encoder on the encode thread: Metal 4 snapshots a table at each draw
// and dispatch.
struct Mtl4Tables {
  WMT::Reference<WMT::MTL4ArgumentTable> vertex, fragment, object, mesh, compute;
};

// part 3: a chunk's binding-state counters, into the METAL4 section (encode thread)
void metal4EncodeCounters(const WMTMtl4BindingState &state);

// gw2 (#242), with DXMT_STATS: a drawable's present, `at_ns` after its encode, for PRESENT presentat;
// `before_pass`: its pass's commit hadn't completed (METAL4 beforepass). Any thread.
void metal4PresentCounted(bool before_pass, uint64_t at_ns);

class Mtl4Queue {
public:
  // gw2 (#204): the most allocators the pool keeps idle (DXMT_MTL4_ALLOCATOR_POOL=1)
  static constexpr size_t kIdleAllocators = 8;

  // `slots`: the chunk ring's size. `allocator_pool`: gw2 (#204), DXMT_MTL4_ALLOCATOR_POOL=1 (the
  // caller reads it once per queue). Check the result with valid().
  Mtl4Queue(WMT::Device device, uint32_t slots, bool allocator_pool = false);
  ~Mtl4Queue();

  Mtl4Queue(const Mtl4Queue &) = delete;
  Mtl4Queue &operator=(const Mtl4Queue &) = delete;

  // the queue, its event, every slot's allocator (with the pool: its first one) and the first
  // command buffers were created
  bool
  valid() const {
    return ready_;
  }

  WMT::MTL4CommandQueue
  queue() const {
    return queue_;
  }

  // gw2 (#204): this queue's command allocators alive now (the slots', in use and idle), and the sum
  // of their sizes at their last reset
  uint32_t
  allocators() const {
    return allocators_.load(std::memory_order_relaxed);
  }
  uint64_t
  allocatorBytes() const {
    return allocator_bytes_.load(std::memory_order_relaxed);
  }
  // The allocator chunk `seq`'s command buffers take their memory from; null with the pool before
  // its first begin() and after its retire()
  WMT::MTL4CommandAllocator
  allocator(uint64_t seq) {
    return slot(seq).allocator.ref;
  }

  // encode thread
  Mtl4Tables &
  tables() {
    return tables_;
  }

  // --- encode thread, chunk `seq`

  // Begins the chunk's next command buffer on its slot's allocator (the first of the chunk, or the
  // one after commit()). The record stays valid until retire(seq). gw2 (#204), with the pool: the
  // chunk's first begin() takes the allocator all of its command buffers use.
  Mtl4Commit &begin(uint64_t seq);

  // The command buffer begin() opened, or null if none is open.
  Mtl4Commit *
  current(uint64_t seq) {
    return slot(seq).open;
  }

  // Ends and commits the open command buffer of chunk `seq`, with its feedback; before that, the
  // device set's and the slot set's pending changes are committed and the command buffer uses the
  // slot's set (gw2 #153, DXMT_MTL4_DEVICE_RESIDENCY=1: if it has allocations).
  void commit(uint64_t seq);
  // gw2 (#242), DXMT_MTL4_PRESENT=done: as commit(seq), for the open command buffer (one must be open),
  // then the drawable is signalled at the queue and presented (mode, value as MetalDrawable::present)
  // once the commit has completed (MTL4CommandQueue_commitPresent)
  void commitPresent(uint64_t seq, WMT::MetalDrawable drawable, WMTDrawablePresentMode mode, double value);

  // The residency set of chunk `seq`'s slot (WMTMtl4BindingState::residency_set)
  WMT::ResidencySet
  residency(uint64_t seq) {
    return slot(seq).residency;
  }

  // Queue-level event operations: they apply to the command buffers committed after (wait) or
  // before (signal) the call.
  void waitForEvent(WMT::Event event, uint64_t value);
  void signalEvent(WMT::Event event, uint64_t value);

  // --- gw2 (#227): DXMT_MTL4_PRESENT=mtl3

  // Before the first chunk: makes the `frame` and `shown` events. False if Metal gave none; the
  // present then stays Metal 4's.
  bool enableMetal3Present();
  bool
  metal3Present() const {
    return metal3_present_;
  }
  // encode thread: the value of the next Metal 3 present (both events reach it once per present)
  uint64_t
  nextMetal3PresentValue() {
    return ++metal3_value_;
  }
  WMT::SharedEvent
  frameEvent() const {
    return frame_event_;
  }
  WMT::SharedEvent
  shownEvent() const {
    return shown_event_;
  }
  // A committed Metal 3 present command buffer and the value it signals `shown` at
  struct Metal3Present {
    WMT::Reference<WMT::CommandBuffer> cmdbuf;
    uint64_t value = 0;
  };
  // encode thread: chunk `seq` committed `cmdbuf`; the slot holds it until retire(seq)
  void addMetal3Present(uint64_t seq, WMT::CommandBuffer cmdbuf, uint64_t value);
  // finish thread, after wait(seq): chunk `seq`'s Metal 3 present buffers, completed, in commit order
  const std::vector<Metal3Present> &
  metal3Presents(uint64_t seq) {
    return slot(seq).metal3;
  }

  // --- gw2 (#242): DXMT_MTL4_PRESENT=done

  // Before the first chunk: the context presents with commitPresent
  void enablePresentWhenDone();
  bool
  presentWhenDone() const {
    return present_done_;
  }

  // --- finish thread

  // Blocks until every command buffer chunk `seq` committed has completed and its feedback is
  // written; returns them in commit order (valid until retire(seq)). gw2 (#227): the chunk's Metal 3
  // present buffers are waited for first (metal3Presents). gw2 (#242): with DXMT_STATS, a commit that
  // carried a present counts it (metal4PresentCounted).
  const std::vector<Mtl4Commit *> &wait(uint64_t seq);

  // After wait(seq), once nothing reads the records: releases their errors, resets the slot's
  // allocator, and recycles the command buffers. gw2 (#204), with the pool: the reset allocator goes
  // back to it, or is released if kIdleAllocators are idle.
  void retire(uint64_t seq);

  // --- teardown, both threads stopped

  // Blocks until every committed command buffer completed and its feedback is written.
  void waitIdle();

private:
  struct Allocator {
    WMT::Reference<WMT::MTL4CommandAllocator> ref;
    uint64_t bytes = 0; // its size at its last reset (allocatedSize)
  };
  struct Slot {
    Allocator allocator; // gw2 (#204), with the pool: only from the chunk's first begin() to its retire()
    WMT::Reference<WMT::ResidencySet> residency;
    std::vector<Mtl4Commit *> commits; // committed, in order
    Mtl4Commit *open = nullptr;        // begun, not committed
    std::vector<Metal3Present> metal3; // gw2 (#227): committed Metal 3 presents, in order
  };

  Slot &
  slot(uint64_t seq) {
    return slots_[seq % slots_.size()];
  }

  struct Present; // gw2 (#242)
  void submit(uint64_t seq, const Present *present);
  Mtl4Commit *acquire();
  void recycle(Mtl4Commit *commit);
  // gw2 (#204)
  bool newAllocator(Allocator &allocator);
  void releaseAllocator(Allocator &allocator);
  Allocator takeAllocator();
  void giveBackAllocator(Allocator &allocator);
  void waitCommit(Mtl4Commit *commit, uint64_t seq);
  void waitMetal3(const Metal3Present &present);

  WMT::Device device_;
  // gw2 (#227)
  WMT::Reference<WMT::SharedEvent> frame_event_, shown_event_;
  bool metal3_present_ = false;
  bool present_done_ = false; // gw2 (#242)
  uint64_t metal3_value_ = 0; // encode thread
  WMT::Reference<WMT::MTL4CommandQueue> queue_;
  WMT::Reference<WMT::SharedEvent> event_;
  uint64_t next_value_ = 0; // encode thread; the last value committed
  std::atomic<uint64_t> committed_value_{0};
  std::vector<Slot> slots_;
  Mtl4Tables tables_;
  bool ready_ = false;
  bool allocator_pool_ = false; // gw2 (#204)
  std::atomic<uint32_t> allocators_{0};
  std::atomic<uint64_t> allocator_bytes_{0};

  std::mutex pool_mutex_;
  std::vector<std::unique_ptr<Mtl4Commit>> records_; // every record, for stable addresses
  std::vector<Mtl4Commit *> free_;
  std::vector<Allocator> idle_allocators_; // gw2 (#204): the pool's, reset; at most kIdleAllocators
};

} // namespace dxmt::gw2
