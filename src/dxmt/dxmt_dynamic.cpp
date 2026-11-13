#include "dxmt_dynamic.hpp"
#include "dxmt_gw2_map.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_texture.hpp"
#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace dxmt {

namespace {
// gw2 (#251): every live dynamic buffer and dynamic linear texture, for MAP's queued. Listed at
// creation and unlisted at destruction, only with DXMT_STATS on; nothing on the Map path touches it.
// Lock order: this mutex, then a resource's mutex_ (renameQueues). A destructor takes it without its
// own mutex_, so a resource the walk reaches isn't destroyed under it. Never destroyed (resources can
// die while static objects are destroyed at exit).
struct Gw2Listed {
  std::mutex mutex;
  std::unordered_set<DynamicBuffer *> buffers;
  std::unordered_set<DynamicLinearTexture *> textures;
};

Gw2Listed &
gw2Listed() {
  static Gw2Listed *listed = new Gw2Listed();
  return *listed;
}
} // namespace

gw2::RenameQueues
gw2::renameQueues() {
  RenameQueues queues;
  auto &listed = gw2Listed();
  std::lock_guard lock(listed.mutex);
  for (auto *buffer : listed.buffers)
    buffer->gw2AddQueued(queues);
  for (auto *texture : listed.textures)
    texture->gw2AddQueued(queues);
  return queues;
}

DynamicBuffer::DynamicBuffer(Buffer *buffer, Flags<BufferAllocationFlag> flags) :
    buffer(buffer),
    flags_(flags),
    name_(buffer->current()) {
  if (gw2::statsEnabled()) {
    std::lock_guard lock(gw2Listed().mutex);
    gw2Listed().buffers.insert(this);
  }
}

DynamicBuffer::~DynamicBuffer() {
  if (gw2::statsEnabled()) {
    std::lock_guard lock(gw2Listed().mutex);
    gw2Listed().buffers.erase(this);
  }
}

void
DynamicBuffer::gw2AddQueued(gw2::RenameQueues &queues) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  for (auto &entry : fifo) {
    queues.pages++;
    queues.bytes += entry.allocation->pageLength();
    queues.oldest = std::min(queues.oldest, entry.will_free_at);
  }
}

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
    fifo.pop_front();
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
    fifo.push_back(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
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
    fifo.push_back(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
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
  fifo.push_back(QueueEntry{.allocation = std::move(allocation), .will_free_at = current_seq_id});
}

DynamicLinearTexture::DynamicLinearTexture(Texture *texture, Flags<TextureAllocationFlag> flags) :
    texture(texture),
    flags_(flags),
    name_(texture->current()) {
  if (gw2::statsEnabled()) {
    gw2_image_bytes_ = texture->gw2BytesPerImage();
    std::lock_guard lock(gw2Listed().mutex);
    gw2Listed().textures.insert(this);
  }
}

DynamicLinearTexture::~DynamicLinearTexture() {
  if (gw2::statsEnabled()) {
    std::lock_guard lock(gw2Listed().mutex);
    gw2Listed().textures.erase(this);
  }
}

void
DynamicLinearTexture::gw2AddQueued(gw2::RenameQueues &queues) {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  for (auto &entry : fifo) { // every allocation of a linear texture is one image
    queues.pages++;
    queues.bytes += gw2_image_bytes_;
    queues.oldest = std::min(queues.oldest, entry.will_free_at);
  }
}

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
    fifo.pop_front();
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
    fifo.push_back(QueueEntry{.allocation = std::move(name_), .will_free_at = current_seq_id});
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
  fifo.push_back(QueueEntry{.allocation = std::move(allocation), .will_free_at = current_seq_id});
}

} // namespace dxmt
