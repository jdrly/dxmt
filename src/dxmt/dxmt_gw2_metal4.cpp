#include "dxmt_gw2_metal4.hpp"
#include "dxmt_gw2_lifetime.hpp"
#include "dxmt_gw2_residency.hpp"
#include "dxmt_gw2_switches.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>
#include <format>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace dxmt::gw2 {

namespace {

enum class Metal4 { Unset, Off, On, Unsupported };

std::atomic<Metal4> g_metal4{Metal4::Unset};

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

namespace {
// gw2 (#60): =1 only; not on the startup line (it never was). CreateSwapChain reads it too, each time.
sw::Switch<bool> kMetalfxSpatialSwapchain{
    "DXMT_METALFX_SPATIAL_SWAPCHAIN", false, sw::legacy::exactlyOne, sw::Line::Unlisted
};
// gw2 (#154)
sw::Switch<bool> kMtl4TimestampReuse{"DXMT_MTL4_TIMESTAMP_REUSE", false, sw::legacy::exactlyOne};
} // namespace

bool
metalfxSpatialSwapchain() {
  return kMetalfxSpatialSwapchain();
}

bool
mtl4TimestampReuse() {
  static const bool on = kMtl4TimestampReuse();
  return on;
}

bool
metal4Requested() {
  // gw2 (#32): on by default (2026-11-04); =0 restores Metal 3 exactly
  static const bool on = env::getEnvVar("DXMT_METAL4") != "0";
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
  if (metalfxSpatialSwapchain()) {
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
  g_metal4.store(Metal4::On, std::memory_order_release);
}

void
metal4Failed(const char *what) {
  g_metal4.store(Metal4::Unsupported, std::memory_order_release);
  Logger::warn(std::format("Metal 4: unavailable ({} failed), staying on Metal 3", what));
}

void
metal4Untested(const char *what) {
  static std::mutex mutex;
  static std::vector<std::string> seen;
  std::lock_guard lock(mutex);
  if (std::find(seen.begin(), seen.end(), what) != seen.end())
    return;
  seen.emplace_back(what);
  Logger::warn(std::format("Metal 4: {} run on the Metal 4 path through the packet replay, untested there", what));
}

} // namespace dxmt::gw2
