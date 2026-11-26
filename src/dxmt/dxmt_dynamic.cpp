#include "dxmt_dynamic.hpp"
#include "dxmt_texture.hpp"
#include <algorithm>

namespace dxmt {
DynamicBuffer::DynamicBuffer(Buffer *buffer, Flags<BufferAllocationFlag> flags) :
    buffer(buffer),
    flags_(flags),
    name_(buffer->current()) {}

void
DynamicBuffer::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
DynamicBuffer::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<BufferAllocation>
DynamicBuffer::allocate(uint64_t coherent_seq_id, uint32_t *gw2_result) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  uint32_t result = 0;
  auto ret = takeFree(coherent_seq_id, result);
  if (gw2_result)
    *gw2_result = result;
  return ret;
}

// gw2 (#25): the oldest free allocation, else a new one (the caller holds mutex_). A free allocation
// that doesn't fit is dropped: a smaller page than page_ (growth), or other flags (a default-usage
// buffer's first allocation, once DXMT_MAP_FAST renames it into suballocated pages). Without
// either, it behaves as upstream's allocate().
Rc<BufferAllocation>
DynamicBuffer::takeFree(uint64_t coherent_seq_id, uint32_t &result) {
  while (!fifo.empty() && fifo.front().will_free_at <= coherent_seq_id) {
    Rc<BufferAllocation> ret = std::move(fifo.front().allocation);
    fifo.pop();
    if (ret->flags() == flags_ && (!grow_length_ || ret->pageLength() >= page_))
      return ret;
    result |= RenameDropped;
  }
  result |= RenameCreated;
  return buffer->allocate(flags_, page_);
}

uint32_t
DynamicBuffer::renameImmediate(uint64_t current_seq_id, uint64_t coherent_seq_id) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  uint32_t result = 0;
  if (grow_length_ && name_seq_id_ == current_seq_id) {
    // the next page holds at least two (16-aligned, at least 256-byte) suballocations
    uint32_t sub = (std::max(grow_length_, 256u) + 15) & ~15u;
    uint32_t next = page_ * 2;
    while (next < sub * 2)
      next *= 2;
    if (next <= kMaxGrowPage) {
      page_ = next;
      result |= RenameGrown;
    }
  }
  Rc<BufferAllocation> next = takeFree(coherent_seq_id, result);
  if (!owned_by_command_list_)
    fifo.push(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
  name_ = std::move(next);
  name_suballocation_ = 0;
  owned_by_command_list_ = false;
  name_seq_id_ = current_seq_id;
  return result;
}

void
DynamicBuffer::updateImmediateName(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation, uint32_t suballocation, bool owned_by_command_list) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (!owned_by_command_list_)
    fifo.push(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
  name_ = std::move(allocation);
  name_suballocation_ = suballocation;
  owned_by_command_list_ = owned_by_command_list;
}

void
DynamicBuffer::recycle(uint64_t current_seq_id, Rc<BufferAllocation> &&allocation) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (owned_by_command_list_) {
    if (name_.ptr() == allocation.ptr()) {
      owned_by_command_list_ = false;
      auto _ = std::move(allocation);
      return;
    }
  }
  fifo.push(QueueEntry{.allocation = std::move(allocation), .will_free_at = current_seq_id});
}

DynamicLinearTexture::DynamicLinearTexture(Texture *texture, Flags<TextureAllocationFlag> flags) :
    texture(texture),
    flags_(flags),
    name_(texture->current()) {}

void
DynamicLinearTexture::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
DynamicLinearTexture::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<TextureAllocation>
DynamicLinearTexture::allocate(uint64_t coherent_seq_id) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  Rc<TextureAllocation> ret;
  for (;;) {
    if (fifo.empty()) {
      break;
    }
    auto entry = fifo.front();
    if (entry.will_free_at > coherent_seq_id) {
      break;
    }
    ret = std::move(entry.allocation);
    fifo.pop();
    break;
  }
  if (!ret.ptr())
    ret = texture->allocate(flags_);
  return ret;
}

void
DynamicLinearTexture::updateImmediateName(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation, bool owned_by_command_list) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (!owned_by_command_list_)
    fifo.push(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
  name_ = std::move(allocation);
  owned_by_command_list_ = owned_by_command_list;
}

void
DynamicLinearTexture::recycle(uint64_t current_seq_id, Rc<TextureAllocation> &&allocation) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  if (owned_by_command_list_) {
    if (name_.ptr() == allocation.ptr()) {
      owned_by_command_list_ = false;
      auto _ = std::move(allocation);
      return;
    }
  }
  fifo.push(QueueEntry{.allocation = std::move(allocation), .will_free_at = current_seq_id});
}

} // namespace dxmt
