#include "dxmt_dynamic.hpp"
#include "dxmt_gw2_map.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_texture.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <mutex>
#include <vector>

namespace dxmt {

namespace {
// gw2 (#251): every live dynamic buffer and dynamic linear texture, for MAP's queued. Listed at
// creation and unlisted at destruction (swapped with the last one), only with DXMT_STATS on; nothing
// on the Map path touches it. The first listing starts a thread that walks the lists with 2 s between
// walks, off the presenting thread, kBatch resources per hold of the mutex (what a create or destroy
// can wait for). Lock order: this mutex, then a resource's mutex_. A destructor takes it without its
// own mutex_, so a resource the walk reaches isn't destroyed under it. A resource that dies between
// batches can move one not yet walked to a walked slot: that walk misses it. Never destroyed
// (resources can die while static objects are destroyed at exit).
struct Gw2Listed {
  std::mutex mutex;
  std::vector<DynamicBuffer *> buffers;
  std::vector<DynamicLinearTexture *> textures;
  bool started = false;
  std::mutex result_mutex;
  gw2::RenameQueues result; // the last walk's
};
constexpr size_t kBatch = 256;

Gw2Listed &
gw2Listed() {
  static Gw2Listed *listed = new Gw2Listed();
  return *listed;
}

template <typename T>
void
gw2Walk(std::vector<T *> &list, gw2::RenameQueues &queues) {
  for (size_t i = 0;; i += kBatch) {
    std::lock_guard lock(gw2Listed().mutex);
    if (i >= list.size())
      return;
    for (size_t j = i; j < std::min(list.size(), i + kBatch); j++)
      list[j]->gw2AddQueued(queues);
  }
}

void
gw2WalkThread() {
  env::setThreadName("dxmt-rename-queues");
  auto &listed = gw2Listed();
  for (;;) {
    Sleep(2000);
    gw2::RenameQueues queues;
    gw2Walk(listed.buffers, queues);
    gw2Walk(listed.textures, queues);
    // read after the walk: every queued will_free_at is at most this id
    uint64_t seq = gw2::mapSeqId();
    queues.age = queues.pages && seq > queues.oldest ? seq - queues.oldest : 0;
    std::lock_guard lock(listed.result_mutex);
    listed.result = queues;
  }
}

template <typename T>
void
gw2List(std::vector<T *> &list, T *resource) {
  auto &listed = gw2Listed();
  std::lock_guard lock(listed.mutex);
  resource->gw2_list_index_ = list.size();
  list.push_back(resource);
  if (!listed.started) {
    listed.started = true;
    dxmt::thread([] { gw2WalkThread(); }).detach();
  }
}

template <typename T>
void
gw2Unlist(std::vector<T *> &list, T *resource) {
  std::lock_guard lock(gw2Listed().mutex);
  size_t i = resource->gw2_list_index_;
  list[i] = list.back();
  list[i]->gw2_list_index_ = i;
  list.pop_back();
}
} // namespace

gw2::RenameQueues
gw2::renameQueues() {
  auto &listed = gw2Listed();
  std::lock_guard lock(listed.result_mutex);
  return listed.result;
}

DynamicBuffer::DynamicBuffer(Buffer *buffer, Flags<BufferAllocationFlag> flags) :
    buffer(buffer),
    flags_(flags),
    name_(buffer->current()) {
  if (gw2::statsEnabled())
    gw2List(gw2Listed().buffers, this);
}

DynamicBuffer::~DynamicBuffer() {
  if (gw2::statsEnabled())
    gw2Unlist(gw2Listed().buffers, this);
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
    gw2List(gw2Listed().textures, this);
  }
}

DynamicLinearTexture::~DynamicLinearTexture() {
  if (gw2::statsEnabled())
    gw2Unlist(gw2Listed().textures, this);
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
