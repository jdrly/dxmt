#include "dxmt_gw2_residency.hpp"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>
#include <format>
#include <mutex>
#include <string>

namespace dxmt::gw2 {

namespace {

// The process's set. Never destroyed: allocations can still be freed (and leave the set) while
// static objects are destroyed at exit.
struct ResidencyState {
  std::mutex mutex;
  std::atomic<bool> active{false};
  std::atomic<bool> dirty{false};
  bool tried = false;
  obj_handle_t device = 0;
  WMT::Reference<WMT::ResidencySet> set;
  // under the mutex
  uint64_t resident = 0; // DXMT allocations in the set
  uint64_t bytes = 0;
  // window totals
  std::atomic<uint64_t> adds{0}, removes{0}, commits{0};
};

ResidencyState &
state() {
  static ResidencyState *s = new ResidencyState();
  return *s;
}

std::string
residencySection(double, uint32_t) {
  auto &s = state();
  if (!s.active.load(std::memory_order_acquire))
    return {};
  uint64_t resident, bytes;
  {
    std::lock_guard lock(s.mutex);
    resident = s.resident;
    bytes = s.bytes;
  }
  return std::format(
      "resident={} mb={:.1f} adds={} removes={} commits={} devmb={:.1f}", resident, bytes / 1e6,
      s.adds.exchange(0, std::memory_order_relaxed), s.removes.exchange(0, std::memory_order_relaxed),
      s.commits.exchange(0, std::memory_order_relaxed), MTLDevice_currentAllocatedSize(s.device) / 1e6
  );
}

SectionRegistrar s_residency("RESIDENCY", &residencySection);

} // namespace

bool
residencySetRequested() {
  static const bool on = env::getEnvVar("DXMT_RESIDENCY_SET") != "0";
  return on;
}

bool
residencyActive() {
  return state().active.load(std::memory_order_acquire);
}

const char *
residencySetState() {
  if (!residencySetRequested())
    return "0";
  return state().active.load(std::memory_order_acquire) ? "1" : "failed";
}

void
residencyAttachQueue(WMT::Device device, WMT::CommandQueue queue) {
  if (!residencySetRequested()) {
    Logger::info("Residency: off (DXMT_RESIDENCY_SET=0): a useResource per resource and encoder, as upstream");
    return;
  }
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.tried) {
    s.tried = true;
    uint64_t major = 0, minor = 0;
    WMTGetOSVersion(&major, &minor, nullptr);
    if (major < 15) {
      Logger::warn(std::format(
          "Residency: off: residency sets need macOS 15 (this is {}.{}); a useResource per resource and encoder",
          major, minor
      ));
      return;
    }
    WMT::Error err;
    s.set = device.newResidencySet(4096, err);
    if (!s.set) {
      Logger::warn(std::format(
          "Residency: off: newResidencySet failed ({}); a useResource per resource and encoder",
          err ? err.description().getUTF8String() : std::string("no error")
      ));
      return;
    }
    s.device = device.handle;
    s.active.store(true, std::memory_order_release);
  }
  if (!s.set)
    return; // failed before; logged then
  if (device.handle != s.device) {
    Logger::warn("Residency: off for this device: the set belongs to another Metal device; a useResource per "
                 "resource and encoder");
    return;
  }
  queue.addResidencySet(s.set);
  Logger::info(std::format(
      "Residency: one residency set on (DXMT_RESIDENCY_SET=1), attached to this device's queue: {} allocations{} "
      "resident. Buffers and textures enter it when created and leave once their last chunk has retired; "
      "makeResident emits no useResource for them",
      s.resident, statsEnabled() ? std::format(", {:.1f} MB", s.bytes / 1e6) : std::string()
  ));
}

bool
residencyAdd(WMT::Device device, const obj_handle_t *allocations, uint32_t count, uint64_t bytes) {
  auto &s = state();
  if (!s.active.load(std::memory_order_acquire) || device.handle != s.device)
    return false;
  obj_handle_t handles[2];
  uint32_t n = 0;
  for (uint32_t i = 0; i < count && n < 2; i++) {
    if (allocations[i])
      handles[n++] = allocations[i];
  }
  if (!n)
    return false;
  std::lock_guard lock(s.mutex);
  MTLResidencySet_addAllocations(s.set.handle, handles, n);
  s.resident++;
  s.dirty.store(true, std::memory_order_release);
  if (statsEnabled()) {
    s.bytes += bytes;
    s.adds.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

void
residencyRemove(const obj_handle_t *allocations, uint32_t count, uint64_t bytes) {
  auto &s = state();
  obj_handle_t handles[2];
  uint32_t n = 0;
  for (uint32_t i = 0; i < count && n < 2; i++) {
    if (allocations[i])
      handles[n++] = allocations[i];
  }
  if (!n)
    return;
  std::lock_guard lock(s.mutex);
  MTLResidencySet_removeAllocations(s.set.handle, handles, n);
  s.resident--;
  s.dirty.store(true, std::memory_order_release);
  if (statsEnabled()) {
    s.bytes -= bytes;
    s.removes.fetch_add(1, std::memory_order_relaxed);
  }
}

void
residencyBeforeCommit() {
  auto &s = state();
  // An add made before the command buffer's commands were encoded happens-before this load (the
  // allocation reached the encode thread through the queue's own synchronization), so it is seen.
  if (!s.dirty.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(s.mutex);
  if (!s.dirty.load(std::memory_order_relaxed))
    return;
  MTLResidencySet_commit(s.set.handle);
  // cleared only after the commit (adds and removes take the mutex, so none came in between): another
  // queue's thread that sees it clear can't commit a command buffer ahead of this set commit
  s.dirty.store(false, std::memory_order_release);
  if (statsEnabled())
    s.commits.fetch_add(1, std::memory_order_relaxed);
}

} // namespace dxmt::gw2
