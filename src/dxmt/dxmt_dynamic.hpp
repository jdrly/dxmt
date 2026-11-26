#pragma once
#include "dxmt_buffer.hpp"
#include "dxmt_texture.hpp"
#include <queue>

namespace dxmt {

class DynamicBuffer {
public:
  void incRef();
  void decRef();

  // gw2 (#25): gw2_result, if given, gets RenameResult bits (RenameCreated, RenameDropped)
  Rc<BufferAllocation> allocate(uint64_t coherent_seq_id, uint32_t *gw2_result = nullptr);
  void updateImmediateName(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation, uint32_t suballocation, bool owned_by_command_list);
  void recycle(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation);

  uint32_t
  nextSuballocation() { // gw2 (#25): inline, it runs on every Map(WRITE_DISCARD)
    if (name_->hasSuballocatoin(name_suballocation_ + 1)) {
      return ++name_suballocation_;
    }
    return 0;
  }

  // gw2 (#25): what renameImmediate did, for the MAP section
  enum RenameResult : uint32_t {
    RenameCreated = 1, // no free allocation fitted: a new Metal buffer
    RenameGrown = 2,   // the page doubled
    RenameDropped = 4, // free allocations were dropped (a smaller page or other flags)
  };

  // gw2 (#25): DXMT_MAP_FAST's rename at the end of the immediate name's page: allocate() and
  // updateImmediateName() under one lock. With growth on, a name that is used up within the chunk
  // (seq id) it was taken in doubles the page for the next ones, up to kMaxGrowPage; allocate() then
  // drops free allocations of a smaller page. Returns RenameResult bits.
  uint32_t renameImmediate(uint64_t current_seq_id, uint64_t coherent_seq_id);
  // gw2 (#25): turns page growth on (before the buffer is used)
  void
  enableGrowth(uint32_t length) {
    grow_length_ = length;
  }
  static constexpr uint32_t kMaxGrowPage = 65536;

  Rc<BufferAllocation>
  immediateName() {
    return name_;
  }

  uint32_t
  immediateSuballocation() {
    return name_suballocation_;
  }

  void *
  immediateMappedMemory() {
    return name_->mappedMemory(name_suballocation_);
  }

  DynamicBuffer(Buffer *buffer, Flags<BufferAllocationFlag> flags);

  struct QueueEntry {
    Rc<BufferAllocation> allocation;
    uint64_t will_free_at;
  };

  /**
   * readonly
   */
  Buffer *buffer;

private:
  Rc<BufferAllocation> takeFree(uint64_t coherent_seq_id, uint32_t &result);

  Flags<BufferAllocationFlag> flags_;
  std::atomic<uint32_t> refcount_ = {0u};
  std::queue<QueueEntry> fifo;
  dxmt::mutex mutex_;
  Rc<BufferAllocation> name_;
  uint32_t name_suballocation_ = 0;
  bool owned_by_command_list_ = false;
  // gw2 (#25): page growth (renameImmediate)
  uint32_t grow_length_ = 0;            // the buffer's length; 0: growth off
  uint32_t page_ = DXMT_PAGE_SIZE;      // the page new allocations get
  uint64_t name_seq_id_ = ~0ull;        // the chunk renameImmediate took the name in
};

class DynamicLinearTexture {
public:
  void incRef();
  void decRef();

  Rc<TextureAllocation> allocate(uint64_t coherent_seq_id);
  void updateImmediateName(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation, bool owned_by_command_list);
  void recycle(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation);

  Rc<TextureAllocation>
  immediateName() {
    return name_;
  };

  void *
  mappedMemory() {
    return name_->mappedMemory;
  }

  DynamicLinearTexture(Texture *buffer, Flags<TextureAllocationFlag> flags);

  struct QueueEntry {
    Rc<TextureAllocation> allocation;
    uint64_t will_free_at;
  };
  /**
   * readonly
   */
  Texture *texture;

private:
  Flags<TextureAllocationFlag> flags_;
  std::atomic<uint32_t> refcount_ = {0u};
  std::queue<QueueEntry> fifo;
  dxmt::mutex mutex_;
  Rc<TextureAllocation> name_;
  bool owned_by_command_list_ = false;
};

} // namespace dxmt
