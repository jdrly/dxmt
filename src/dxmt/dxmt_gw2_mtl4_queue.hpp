#pragma once

// gw2 (#32): Metal 4 submission for a DXMT command queue (DXMT_METAL4=1, jdrly/dxmt #32 part 2).
//
// - One MTL4CommandQueue, with the device's residency set attached (Metal 4 has no useResource).
// - One MTL4CommandAllocator per chunk slot (the ring's size, DXMT_CHUNKS): a chunk's command buffers
//   take their memory from their slot's allocator, which the finish thread resets when it retires
//   the chunk, after the GPU completed every buffer of it.
// - A chunk commits one command buffer, or several (the present split commits the frame before
//   nextDrawable, and part 3 may split at queue-level events). Each commit carries its own
//   MTL4CommitOptions feedback (GPU start and end, error), written into an Mtl4Commit record, and
//   raises the queue's shared event to the commit's value once written. The finish thread waits on
//   that event instead of waitUntilCompleted.
// - Each slot also has a residency set of its own (part 3): the replays put every allocation the
//   chunk's packets name directly into it (Metal 4 makes nothing resident by itself); each commit
//   commits it and the command buffer uses it; retire() empties it.
// - Command buffers and records are recycled (an MTL4CommandBuffer is reusable after completion).
//
// Threads: begin/commit/waitForEvent/signalEvent on the encode thread, for the chunk being encoded;
// wait/retire on the finish thread, for the oldest chunk not retired; waitIdle at teardown, once
// both have stopped. The queue's existing chunk handshake (ready_for_commit, chunk_ongoing) orders
// a slot's use between them.
//
// The METAL4 section (2 s, with DXMT_STATS): commits= buffers=<per chunk, max> allocmb=<largest
// allocator at reset> slow=<chunks the finish thread waited for over 1 s> errors=.

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

class Mtl4Queue {
public:
  // `slots`: the chunk ring's size. Check the result with valid().
  Mtl4Queue(WMT::Device device, uint32_t slots);
  ~Mtl4Queue();

  Mtl4Queue(const Mtl4Queue &) = delete;
  Mtl4Queue &operator=(const Mtl4Queue &) = delete;

  // the queue, its event, every slot's allocator and the first command buffers were created
  bool
  valid() const {
    return ready_;
  }

  WMT::MTL4CommandQueue
  queue() const {
    return queue_;
  }

  // encode thread
  Mtl4Tables &
  tables() {
    return tables_;
  }

  // --- encode thread, chunk `seq`

  // Begins the chunk's next command buffer on its slot's allocator (the first of the chunk, or the
  // one after commit()). The record stays valid until retire(seq).
  Mtl4Commit &begin(uint64_t seq);

  // The command buffer begin() opened, or null if none is open.
  Mtl4Commit *
  current(uint64_t seq) {
    return slot(seq).open;
  }

  // Ends and commits the open command buffer of chunk `seq`, with its feedback; before that, the
  // device set's and the slot set's pending changes are committed and the command buffer uses the
  // slot's set.
  void commit(uint64_t seq);

  // The residency set of chunk `seq`'s slot (WMTMtl4BindingState::residency_set)
  WMT::ResidencySet
  residency(uint64_t seq) {
    return slot(seq).residency;
  }

  // Queue-level event operations: they apply to the command buffers committed after (wait) or
  // before (signal) the call.
  void waitForEvent(WMT::Event event, uint64_t value);
  void signalEvent(WMT::Event event, uint64_t value);

  // --- finish thread

  // Blocks until every command buffer chunk `seq` committed has completed and its feedback is
  // written; returns them in commit order (valid until retire(seq)).
  const std::vector<Mtl4Commit *> &wait(uint64_t seq);

  // After wait(seq), once nothing reads the records: releases their errors, resets the slot's
  // allocator, and recycles the command buffers.
  void retire(uint64_t seq);

  // --- teardown, both threads stopped

  // Blocks until every committed command buffer completed and its feedback is written.
  void waitIdle();

private:
  struct Slot {
    WMT::Reference<WMT::MTL4CommandAllocator> allocator;
    WMT::Reference<WMT::ResidencySet> residency;
    std::vector<Mtl4Commit *> commits; // committed, in order
    Mtl4Commit *open = nullptr;        // begun, not committed
  };

  Slot &
  slot(uint64_t seq) {
    return slots_[seq % slots_.size()];
  }

  Mtl4Commit *acquire();
  void recycle(Mtl4Commit *commit);
  void waitCommit(Mtl4Commit *commit, uint64_t seq);

  WMT::Device device_;
  WMT::Reference<WMT::MTL4CommandQueue> queue_;
  WMT::Reference<WMT::SharedEvent> event_;
  uint64_t next_value_ = 0; // encode thread; the last value committed
  std::atomic<uint64_t> committed_value_{0};
  std::vector<Slot> slots_;
  Mtl4Tables tables_;
  bool ready_ = false;

  std::mutex pool_mutex_;
  std::vector<std::unique_ptr<Mtl4Commit>> records_; // every record, for stable addresses
  std::vector<Mtl4Commit *> free_;
};

} // namespace dxmt::gw2
