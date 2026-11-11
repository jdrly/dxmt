#pragma once

// gw2 (#148): the render thread's window of the current chunk's CPU command heap. Before it, every
// command a chunk recorded took its memory from the queue's ring allocator (CommandChunk::emitcc ->
// allocate_cpu_heap -> CommandQueue::AllocateCommandData -> RingBumpState::allocate), out of line.
// Now an emit aligns `cur`, checks it against `end` and returns the place; the ring is called once a
// slab (kSlab), not once a command.
//
// Why a slab belongs to one chunk: the ring stamps a block with the sequence of the chunk that last
// allocated from it, and frees or reuses the block once that chunk has completed. A slab taken while
// chunk N was current is stamped N only. If chunk N + 1 kept writing into it, its commands would sit
// in memory the ring hands out again as soon as N completes, while N + 1 may not have been encoded.
// So the window is emptied whenever the current chunk changes: CommandQueue::CommitCurrentChunk
// calls reset(). Pure C++: tests/gw2/emit_window_test.cpp builds it natively.

#include <cstddef>
#include <cstdint>

namespace dxmt::gw2 {

template <typename Chunk> class EmitWindow {
public:
  // A refill's size. The ring's blocks are 4 MB; at most this much less a command is left unused
  // when the chunk commits.
  static constexpr size_t kSlab = 0x4000;

  struct Place {
    Chunk *chunk;
    void *ptr;
  };

  // `size` bytes aligned to `alignment` (a power of two) in the current chunk's heap. `queue` gives
  // the current chunk (CurrentChunk()) and the ring's memory (AllocateCommandData(size, alignment)),
  // called only when the window runs out.
  template <typename Queue>
  Place
  place(Queue &queue, size_t size, size_t alignment) {
    uintptr_t p = (cur_ + (alignment - 1)) & ~uintptr_t(alignment - 1);
    if (__builtin_expect(p + size <= end_, 1)) {
      cur_ = p + size;
      return {chunk_, reinterpret_cast<void *>(p)};
    }
    return refill(queue, size, alignment);
  }

  // The current chunk changes: nothing taken so far may be used again. An empty window (cur = end
  // = 0) sends the next place() to refill, which reads the new current chunk.
  void
  reset() {
    cur_ = 0;
    end_ = 0;
  }

private:
  template <typename Queue>
  [[gnu::noinline]] Place
  refill(Queue &queue, size_t size, size_t alignment) {
    chunk_ = queue.CurrentChunk();
    // a command that wouldn't fit a fresh slab takes its own memory and leaves the window as it is
    if (size + alignment > kSlab)
      return {chunk_, queue.AllocateCommandData(size, alignment)};
    auto p = reinterpret_cast<uintptr_t>(queue.AllocateCommandData(kSlab, alignment));
    cur_ = p + size;
    end_ = p + kSlab;
    return {chunk_, reinterpret_cast<void *>(p)};
  }

  Chunk *chunk_ = nullptr;
  uintptr_t cur_ = 0;
  uintptr_t end_ = 0;
};

} // namespace dxmt::gw2
