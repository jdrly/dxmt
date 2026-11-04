#include "dxmt_gw2_deps.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include "log/log.hpp"
#include <atomic>
#include <format>
#include <string>

namespace dxmt::gw2 {

namespace {

sw::Switch<bool> kDependencyCompression{"DXMT_DEPENDENCY_COMPRESSION", false, sw::legacy::exactlyOne};

// 0: no queue yet, 1: legacy (off), 2: compressed, 3: requested but the queue is on Metal 3
std::atomic<int> g_state{0};

sw::Effective kDependencyCompressionLine{"DXMT_DEPENDENCY_COMPRESSION", [] {
  switch (g_state.load(std::memory_order_relaxed)) {
  case 2: return std::string("1");
  case 3: return std::string("unsupported");
  case 1: return std::string("0");
  default: return sw::formatValue(kDependencyCompression());
  }
}};

std::string
section(double, uint32_t frames) {
  auto &c = dependencyCounters();
  auto x = [](std::atomic<uint64_t> &v) { return v.exchange(0, std::memory_order_relaxed); };
  double f = frames ? double(frames) : 1.0;
  auto encoders = x(c.encoders), strong = x(c.strong), full = x(c.full), waits = x(c.waits);
  auto checkpoints = x(c.checkpoints), gaps = x(c.gaps), ns = x(c.ns);
  int state = g_state.load(std::memory_order_relaxed);
  return std::format(
      "policy={} stages/f={:.1f} strong/f={:.1f} full/f={:.1f} waits/f={:.1f} checkpoints/f={:.2f} gaps={} "
      "us/f={:.1f}",
      state == 2 ? "compressed" : "legacy", encoders / f, strong / f, full / f, waits / f, checkpoints / f, gaps,
      ns / 1e3 / f
  );
}

SectionRegistrar g_section("DEPS", section);

} // namespace

bool
dependencyCompressionAttach(bool metal4) {
  bool requested = kDependencyCompression();
  bool compressed = requested && metal4;
  int state = compressed ? 2 : requested ? 3 : 1;
  if (g_state.exchange(state, std::memory_order_relaxed) != state) {
    if (compressed)
      Logger::info("Dependency compression: on (DXMT_DEPENDENCY_COMPRESSION=1): encoder fences follow a checkpoint "
                   "chain instead of the per-encoder weak window (Metal 4)");
    else if (requested)
      Logger::info("Dependency compression: requested but the queue is on Metal 3; the legacy weak window stays");
  }
  return compressed;
}

} // namespace dxmt::gw2
