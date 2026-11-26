#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_residency.hpp"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <thread>

namespace dxmt::gw2 {

namespace {

// METAL4 window totals (2 s); maxima reset with the window
std::atomic<uint64_t> g_commits{0}, g_max_buffers{0}, g_max_alloc_bytes{0}, g_slow{0}, g_errors{0};
// part 3: the replays' counters (WMTMtl4BindingState)
std::atomic<uint64_t> g_res_adds{0}, g_missed{0}, g_unsupported{0}, g_bytes_overflow{0}, g_blit_barriers{0};
std::atomic<bool> g_any{false};
std::atomic<bool> g_present_mtl3{false}; // gw2 (#227): DXMT_MTL4_PRESENT=mtl3 in use
// gw2 (#204): every queue's command allocators alive now, and their sizes at their last reset (not
// reset with the window)
std::atomic<uint64_t> g_allocators{0}, g_alloc_held_bytes{0};

void
atomicMax(std::atomic<uint64_t> &target, uint64_t value) {
  uint64_t seen = target.load(std::memory_order_relaxed);
  while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed))
    ;
}

std::string
metal4Section(double, uint32_t) {
  if (!g_any.load(std::memory_order_relaxed))
    return {};
  return std::format(
      "commits={} buffers={} allocmb={:.2f} allocators={} allocheld={:.2f} slow={} errors={} resadds={} missed={} "
      "unsupported={} bytesover={} blitbar={} present={}",
      g_commits.exchange(0, std::memory_order_relaxed), g_max_buffers.exchange(0, std::memory_order_relaxed),
      g_max_alloc_bytes.exchange(0, std::memory_order_relaxed) / 1e6, g_allocators.load(std::memory_order_relaxed),
      g_alloc_held_bytes.load(std::memory_order_relaxed) / 1e6, g_slow.exchange(0, std::memory_order_relaxed),
      g_errors.exchange(0, std::memory_order_relaxed), g_res_adds.exchange(0, std::memory_order_relaxed),
      g_missed.exchange(0, std::memory_order_relaxed), g_unsupported.exchange(0, std::memory_order_relaxed),
      g_bytes_overflow.exchange(0, std::memory_order_relaxed), g_blit_barriers.exchange(0, std::memory_order_relaxed),
      g_present_mtl3.load(std::memory_order_relaxed) ? "mtl3" : "mtl4"
  );
}

SectionRegistrar s_metal4("METAL4", &metal4Section);

// part 3: buffers 31 (Metal's maximum, airconv's [[buffer(n)]] are below it), textures 32 (DXMT binds
// textures through argument buffers; direct ones are internal passes'), no samplers (the same)
WMT::Reference<WMT::MTL4ArgumentTable>
newTable(WMT::Device device) {
  WMTArgTableInfo info{};
  info.max_buffers = WMT_MTL4_MAX_BUFFERS;
  info.max_textures = 32;
  info.max_samplers = 0;
  WMT::Error err;
  auto table = device.newArgumentTable(info, err);
  if (!table)
    Logger::warn(std::format(
        "Metal 4: newArgumentTable failed ({})", err ? err.description().getUTF8String() : std::string("no error")
    ));
  return table;
}

} // namespace

void
metal4EncodeCounters(const WMTMtl4BindingState &state) {
  if (!statsEnabled())
    return;
  g_res_adds.fetch_add(state.residency_adds, std::memory_order_relaxed);
  g_missed.fetch_add(state.use_resource_dropped, std::memory_order_relaxed);
  g_unsupported.fetch_add(state.unsupported, std::memory_order_relaxed);
  g_bytes_overflow.fetch_add(state.bytes_overflow, std::memory_order_relaxed);
  g_blit_barriers.fetch_add(state.blit_barriers, std::memory_order_relaxed);
}

Mtl4Queue::Mtl4Queue(WMT::Device device, uint32_t slots, bool allocator_pool) :
    device_(device),
    allocator_pool_(allocator_pool) {
  queue_ = device.newMTL4CommandQueue();
  event_ = device.newSharedEvent();
  if (!queue_ || !event_)
    return;
  slots_.resize(slots);
  for (auto &slot : slots_) {
    if (!allocator_pool_ && !newAllocator(slot.allocator))
      return; // valid() is false
    WMT::Error err;
    slot.residency = device.newResidencySet(256, err);
    if (!slot.residency)
      return; // valid() is false
  }
  // gw2 (#204): the pool's first allocator, so a device that makes none fails here, while the
  // device can still stay on Metal 3
  if (allocator_pool_) {
    Allocator first;
    if (!newAllocator(first))
      return; // valid() is false
    idle_allocators_.push_back(std::move(first));
  }
  tables_.vertex = newTable(device);
  tables_.fragment = newTable(device);
  tables_.object = newTable(device);
  tables_.mesh = newTable(device);
  tables_.compute = newTable(device);
  if (!tables_.vertex || !tables_.fragment || !tables_.object || !tables_.mesh || !tables_.compute)
    return; // valid() is false
  // a command buffer per slot, and two spare for the present split: a failure here still leaves the
  // device on Metal 3 (nothing was committed yet); later ones are fatal (acquire)
  for (uint32_t i = 0; i < slots + 2; i++) {
    auto record = std::make_unique<Mtl4Commit>();
    record->cmdbuf = device.newMTL4CommandBuffer();
    if (!record->cmdbuf)
      return; // valid() is false
    free_.push_back(record.get());
    records_.push_back(std::move(record));
  }
  ready_ = true;
  g_any.store(true, std::memory_order_relaxed);
}

Mtl4Queue::~Mtl4Queue() {
  waitIdle();
  for (auto &slot : slots_) {
    for (auto *commit : slot.commits)
      if (commit->feedback.error)
        WMT::Object{commit->feedback.error}.release();
    slot.commits.clear();
    slot.open = nullptr; // begun, never committed: nothing of it runs
    if (slot.allocator.ref)
      releaseAllocator(slot.allocator); // nothing runs any more (waitIdle)
  }
  for (auto &allocator : idle_allocators_)
    releaseAllocator(allocator);
}

// gw2 (#204): the allocator counts follow every allocator made and released, with or without the pool
bool
Mtl4Queue::newAllocator(Allocator &allocator) {
  allocator.ref = device_.newCommandAllocator();
  allocator.bytes = 0;
  if (!allocator.ref)
    return false;
  allocators_.fetch_add(1, std::memory_order_relaxed);
  g_allocators.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void
Mtl4Queue::releaseAllocator(Allocator &allocator) {
  allocators_.fetch_sub(1, std::memory_order_relaxed);
  g_allocators.fetch_sub(1, std::memory_order_relaxed);
  allocator_bytes_.fetch_sub(allocator.bytes, std::memory_order_relaxed);
  g_alloc_held_bytes.fetch_sub(allocator.bytes, std::memory_order_relaxed);
  allocator = {};
}

// gw2 (#204), encode thread, a chunk's first begin(): an idle allocator of the pool, or a new one
Mtl4Queue::Allocator
Mtl4Queue::takeAllocator() {
  {
    std::lock_guard lock(pool_mutex_);
    if (!idle_allocators_.empty()) {
      Allocator allocator = std::move(idle_allocators_.back());
      idle_allocators_.pop_back();
      return allocator;
    }
  }
  Allocator allocator;
  if (!newAllocator(allocator)) {
    // as acquire(): earlier chunks may still be on the GPU and this one can't be encoded
    Logger::err(std::format(
        "Metal 4: newCommandAllocator failed with {} allocators alive (DXMT_MTL4_ALLOCATOR_POOL=1); DXMT can't "
        "continue on Metal 4, aborting",
        allocators()
    ));
    std::abort();
  }
  return allocator;
}

// gw2 (#204), finish thread, retire(): `allocator` was reset after its chunk completed
void
Mtl4Queue::giveBackAllocator(Allocator &allocator) {
  {
    std::lock_guard lock(pool_mutex_);
    if (idle_allocators_.size() < kIdleAllocators) {
      idle_allocators_.push_back(std::move(allocator));
      allocator = {};
      return;
    }
  }
  releaseAllocator(allocator);
}

Mtl4Commit *
Mtl4Queue::acquire() {
  {
    std::lock_guard lock(pool_mutex_);
    if (!free_.empty()) {
      auto *commit = free_.back();
      free_.pop_back();
      return commit;
    }
  }
  auto record = std::make_unique<Mtl4Commit>();
  record->cmdbuf = device_.newMTL4CommandBuffer();
  if (!record->cmdbuf) {
    // Earlier chunks may still be on the GPU and this one can't be encoded or committed: stop the
    // process rather than commit nothing and wait for it, or free what the GPU still uses.
    Logger::err(std::format(
        "Metal 4: newCommandBuffer failed after {} command buffers; DXMT can't continue on Metal 4, aborting",
        records_.size()
    ));
    std::abort();
  }
  std::lock_guard lock(pool_mutex_);
  records_.push_back(std::move(record));
  return records_.back().get();
}

void
Mtl4Queue::recycle(Mtl4Commit *commit) {
  if (commit->feedback.error)
    WMT::Object{commit->feedback.error}.release();
  commit->feedback = {};
  commit->value = 0;
  std::lock_guard lock(pool_mutex_);
  free_.push_back(commit);
}

Mtl4Commit &
Mtl4Queue::begin(uint64_t seq) {
  auto &s = slot(seq);
  assert(!s.open && "Metal 4: a command buffer of this chunk is still open");
  auto *commit = acquire();
  if (!s.allocator.ref)
    s.allocator = takeAllocator(); // gw2 (#204): with the pool, the chunk's first command buffer
  commit->cmdbuf.begin(s.allocator.ref);
  s.open = commit;
  return *commit;
}

void
Mtl4Queue::commit(uint64_t seq) {
  auto &s = slot(seq);
  auto *commit = s.open;
  if (!commit)
    return;
  s.open = nullptr;
  residencyBeforeCommit(); // gw2 (#29): the device set's pending adds and removes
  // what this chunk's packets named so far; gw2 (#153), with DXMT_MTL4_DEVICE_RESIDENCY=1: an empty
  // set (everything named was in the device's) is neither committed nor used
  if (!mtl4DeviceResidencyRequested() || s.residency.allocationCount()) {
    s.residency.commit();
    commit->cmdbuf.useResidencySet(s.residency);
  }
  commit->cmdbuf.end();
  commit->value = ++next_value_;
  s.commits.push_back(commit);
  WMT::MTL4CommandBuffer cmdbuf = commit->cmdbuf;
  queue_.commit(&cmdbuf, 1, &commit->feedback, event_, commit->value);
  committed_value_.store(commit->value, std::memory_order_release);
  if (statsEnabled())
    g_commits.fetch_add(1, std::memory_order_relaxed);
}

void
Mtl4Queue::waitForEvent(WMT::Event event, uint64_t value) {
  queue_.waitForEvent(event, value);
}

void
Mtl4Queue::signalEvent(WMT::Event event, uint64_t value) {
  queue_.signalEvent(event, value);
}

bool
Mtl4Queue::enableMetal3Present() {
  frame_event_ = device_.newSharedEvent();
  shown_event_ = device_.newSharedEvent();
  metal3_present_ = frame_event_ && shown_event_;
  if (metal3_present_)
    g_present_mtl3.store(true, std::memory_order_relaxed);
  return metal3_present_;
}

void
Mtl4Queue::addMetal3Present(uint64_t seq, WMT::CommandBuffer cmdbuf, uint64_t value) {
  slot(seq).metal3.push_back(Metal3Present{WMT::Reference<WMT::CommandBuffer>(cmdbuf), value});
}

// gw2 (#227): a Metal 3 present buffer completed. If it failed, `shown` may never reach its value, and
// every later commit of this queue waits for it: the buffer is complete, nothing of it runs any more,
// so the CPU signals `shown` in its place. Its error itself is reported from the chunk's records.
void
Mtl4Queue::waitMetal3(const Metal3Present &present) {
  WMT::CommandBuffer cmdbuf = present.cmdbuf;
  cmdbuf.waitUntilCompleted();
  if (cmdbuf.status() != WMTCommandBufferStatusError)
    return;
  g_errors.fetch_add(1, std::memory_order_relaxed);
  if (shown_event_.signaledValue() >= present.value)
    return;
  shown_event_.signalValue(present.value);
  static std::atomic<uint32_t> s_lines{0};
  if (s_lines.fetch_add(1, std::memory_order_relaxed) < 8)
    Logger::err(std::format(
        "Metal 4: the Metal 3 present command buffer {} failed before signalling; signalled it from the CPU so "
        "the Metal 4 queue goes on (DXMT_MTL4_PRESENT=mtl3; only the first 8 are logged)",
        present.value
    ));
}

void
Mtl4Queue::waitCommit(Mtl4Commit *commit, uint64_t seq) {
  // a GPU fault or hang shows here instead of as a silent stall; Metal 3's waitUntilCompleted
  // blocks the same way, without a line
  static std::atomic<uint32_t> s_slow_lines{0};
  bool slow = false;
  while (!event_.waitUntilSignaledValue(commit->value, 1000)) {
    if (slow)
      continue;
    slow = true;
    g_slow.fetch_add(1, std::memory_order_relaxed);
    if (s_slow_lines.fetch_add(1, std::memory_order_relaxed) < 8)
      Logger::warn(std::format(
          "Metal 4: chunk {} not complete after 1 s (its command buffer's value {}, the queue event at {}); "
          "still waiting",
          seq, commit->value, event_.signaledValue()
      ));
  }
  // The event runs ahead of this record if a later commit's handler raised it first (two handlers
  // at once): the record's own flag says its feedback is written.
  while (!commit->done())
    std::this_thread::yield();
}

const std::vector<Mtl4Commit *> &
Mtl4Queue::wait(uint64_t seq) {
  auto &s = slot(seq);
  // gw2 (#227): the Metal 3 presents first. Each needs only this queue's commits before it, and the
  // commits after it wait for its `shown`, which waitMetal3 signals itself after a failure
  for (auto &present : s.metal3)
    waitMetal3(present);
  for (auto *commit : s.commits) {
    waitCommit(commit, seq);
    if (commit->feedback.error)
      g_errors.fetch_add(1, std::memory_order_relaxed);
  }
  if (statsEnabled())
    atomicMax(g_max_buffers, s.commits.size());
  return s.commits;
}

void
Mtl4Queue::retire(uint64_t seq) {
  auto &s = slot(seq);
  for (auto *commit : s.commits)
    recycle(commit);
  s.commits.clear();
  s.metal3.clear(); // gw2 (#227): completed in wait(seq)
  // nothing of the chunk runs any more: its allocations may leave (and be freed); gw2 (#153), with
  // DXMT_MTL4_DEVICE_RESIDENCY=1: an empty set stays as it is
  if (!mtl4DeviceResidencyRequested() || s.residency.allocationCount()) {
    s.residency.removeAllAllocations();
    s.residency.commit();
  }
  // gw2 (#204): with the pool, a chunk that began no command buffer has no allocator
  if (!s.allocator.ref)
    return;
  // every command buffer that took memory from it has completed (wait), so it may be reset
  uint64_t bytes = s.allocator.ref.reset();
  if (statsEnabled())
    atomicMax(g_max_alloc_bytes, bytes);
  // its size at this reset replaces its last one in the sums (unsigned wrap-around gives the difference)
  allocator_bytes_.fetch_add(bytes - s.allocator.bytes, std::memory_order_relaxed);
  g_alloc_held_bytes.fetch_add(bytes - s.allocator.bytes, std::memory_order_relaxed);
  s.allocator.bytes = bytes;
  // gw2 (#204): reset, so no command buffer uses it any more: back to the pool
  if (allocator_pool_)
    giveBackAllocator(s.allocator);
}

void
Mtl4Queue::waitIdle() {
  if (!event_)
    return;
  // gw2 (#227): the Metal 3 presents first, in present order (each one's `frame` comes after the
  // `shown` of the one before), so a failed one can't leave the queue waiting for `shown`
  std::vector<const Metal3Present *> presents;
  for (auto &slot : slots_)
    for (auto &present : slot.metal3)
      presents.push_back(&present);
  std::sort(presents.begin(), presents.end(), [](auto *a, auto *b) { return a->value < b->value; });
  for (auto *present : presents)
    waitMetal3(*present);
  uint64_t last = committed_value_.load(std::memory_order_acquire);
  if (last)
    event_.waitUntilSignaledValue(last, ~0ull);
  for (auto &slot : slots_)
    for (auto *commit : slot.commits)
      while (!commit->done())
        std::this_thread::yield();
}

} // namespace dxmt::gw2
