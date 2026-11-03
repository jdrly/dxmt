#include "dxmt_gw2_metal4.hpp"
#include "dxmt_gw2_lifetime.hpp"
#include "dxmt_gw2_residency.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>
#include <format>
#include <mutex>

namespace dxmt::gw2 {

namespace {

enum class Metal4 { Unset, Off, On, Unsupported, Incomplete };

std::atomic<Metal4> g_metal4{Metal4::Unset};

// gw2 (#32): part 3 of 4 (the bridge, the queue and submission, the encoders). Present (part 4)
// follows and sets this.
constexpr bool kPortComplete = false;

const char *
supportReason(WMTMetal4Support support) {
  switch (support) {
  case WMTMetal4NoOS:
    return "Metal 4 needs macOS 26";
  case WMTMetal4NoFamily:
    return "the GPU isn't in MTLGPUFamilyMetal4";
  case WMTMetal4NoQueue:
    return "newMTL4CommandQueue failed";
  default:
    return "unknown";
  }
}

} // namespace

bool
metal4Requested() {
  static const bool on = env::getEnvVar("DXMT_METAL4") == "1";
  return on;
}

bool
metal4Active() {
  return g_metal4.load(std::memory_order_acquire) == Metal4::On;
}

const char *
metal4State() {
  switch (g_metal4.load(std::memory_order_acquire)) {
  case Metal4::On:
    return "1";
  case Metal4::Unsupported:
    return "unsupported";
  case Metal4::Incomplete:
    return "incomplete";
  default:
    return "0";
  }
}

void
metal4AttachQueue(WMT::Device device) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  if (g_metal4.load(std::memory_order_relaxed) != Metal4::Unset)
    return;
  if (!metal4Requested()) {
    g_metal4.store(Metal4::Off, std::memory_order_release); // today's path: no call, no line
    return;
  }
  auto support = device.metal4Support();
  if (support != WMTMetal4Supported) {
    g_metal4.store(Metal4::Unsupported, std::memory_order_release);
    Logger::warn(std::format("Metal 4: unavailable ({}), staying on Metal 3", supportReason(support)));
    return;
  }
  if (env::getEnvVar("DXMT_METALFX_SPATIAL_SWAPCHAIN") == "1") {
    g_metal4.store(Metal4::Unsupported, std::memory_order_release);
    Logger::warn("Metal 4: DXMT_METALFX_SPATIAL_SWAPCHAIN=1: MetalFX upscaling stays on Metal 3, staying on Metal 3");
    return;
  }
  if (!lifetime::keepUntilRetired()) {
    g_metal4.store(Metal4::Unsupported, std::memory_order_release);
    Logger::warn("Metal 4: needs DXMT_UNRETAINED=1 (the default): Metal 4 command buffers retain nothing, so the "
                 "chunk must hold what it uses; staying on Metal 3");
    return;
  }
  if (!residencyActive()) {
    g_metal4.store(Metal4::Unsupported, std::memory_order_release);
    Logger::warn(std::format(
        "Metal 4: needs the residency set (DXMT_RESIDENCY_SET={}: Metal 4 encoders have no useResource), staying "
        "on Metal 3",
        residencySetState()
    ));
    return;
  }
  if (!kPortComplete) {
    g_metal4.store(Metal4::Incomplete, std::memory_order_release);
    Logger::warn("Metal 4: not yet complete, staying on Metal 3 (DXMT_METAL4=1; the device supports it; this build "
                 "has the bridge, the queue and the encoders, jdrly/dxmt #32 part 3 of 4)");
    return;
  }
  g_metal4.store(Metal4::On, std::memory_order_release);
}

void
metal4Failed(const char *what) {
  g_metal4.store(Metal4::Unsupported, std::memory_order_release);
  Logger::warn(std::format("Metal 4: unavailable ({} failed), staying on Metal 3", what));
}

} // namespace dxmt::gw2
