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
      "commits={} buffers={} allocmb={:.2f} slow={} errors={} resadds={} missed={} unsupported={} bytesover={} "
      "blitbar={}",
      g_commits.exchange(0, std::memory_order_relaxed), g_max_buffers.exchange(0, std::memory_order_relaxed),
      g_max_alloc_bytes.exchange(0, std::memory_order_relaxed) / 1e6, g_slow.exchange(0, std::memory_order_relaxed),
      g_errors.exchange(0, std::memory_order_relaxed), g_res_adds.exchange(0, std::memory_order_relaxed),
      g_missed.exchange(0, std::memory_order_relaxed), g_unsupported.exchange(0, std::memory_order_relaxed),
      g_bytes_overflow.exchange(0, std::memory_order_relaxed), g_blit_barriers.exchange(0, std::memory_order_relaxed)
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

Mtl4Queue::Mtl4Queue(WMT::Device device, uint32_t slots) : device_(device) {
  queue_ = device.newMTL4CommandQueue();
  event_ = device.newSharedEvent();
  if (!queue_ || !event_)
    return;
  slots_.resize(slots);
  for (auto &slot : slots_) {
    slot.allocator = device.newCommandAllocator();
    WMT::Error err;
    slot.residency = device.newResidencySet(256, err);
    if (!slot.allocator || !slot.residency)
      return; // valid() is false
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
  }
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
  commit->cmdbuf.begin(s.allocator);
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
  // what this chunk's packets named so far; gw2 (#153): an empty set (everything named was in the
  // device's) is neither committed nor used
  if (mtl4ChunkResidency() || s.residency.allocationCount()) {
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
  // nothing of the chunk runs any more: its allocations may leave (and be freed); gw2 (#153): an
  // empty set stays as it is
  if (mtl4ChunkResidency() || s.residency.allocationCount()) {
    s.residency.removeAllAllocations();
    s.residency.commit();
  }
  // every command buffer that took memory from it has completed (wait), so it may be reset
  uint64_t bytes = s.allocator.reset();
  if (statsEnabled())
    atomicMax(g_max_alloc_bytes, bytes);
}

void
Mtl4Queue::waitIdle() {
  if (!event_)
    return;
  uint64_t last = committed_value_.load(std::memory_order_acquire);
  if (last)
    event_.waitUntilSignaledValue(last, ~0ull);
  for (auto &slot : slots_)
    for (auto *commit : slot.commits)
      while (!commit->done())
        std::this_thread::yield();
}

} // namespace dxmt::gw2
