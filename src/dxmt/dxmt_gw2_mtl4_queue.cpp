#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <format>
#include <thread>

namespace dxmt::gw2 {

namespace {

// METAL4 window totals (2 s); maxima reset with the window
std::atomic<uint64_t> g_commits{0}, g_max_buffers{0}, g_max_alloc_bytes{0}, g_slow{0}, g_errors{0};
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
      "commits={} buffers={} allocmb={:.2f} slow={} errors={}", g_commits.exchange(0, std::memory_order_relaxed),
      g_max_buffers.exchange(0, std::memory_order_relaxed),
      g_max_alloc_bytes.exchange(0, std::memory_order_relaxed) / 1e6, g_slow.exchange(0, std::memory_order_relaxed),
      g_errors.exchange(0, std::memory_order_relaxed)
  );
}

SectionRegistrar s_metal4("METAL4", &metal4Section);

} // namespace

Mtl4Queue::Mtl4Queue(WMT::Device device, uint32_t slots) : device_(device) {
  queue_ = device.newMTL4CommandQueue();
  event_ = device.newSharedEvent();
  if (!queue_ || !event_)
    return;
  slots_.resize(slots);
  for (auto &slot : slots_) {
    slot.allocator = device.newCommandAllocator();
    if (!slot.allocator)
      return; // valid() is false
  }
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
