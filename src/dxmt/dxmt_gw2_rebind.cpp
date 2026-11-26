#include "dxmt_gw2_rebind.hpp"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>
#include <format>
#include <string>

namespace dxmt::gw2 {

namespace {

using counter = std::atomic<uint64_t>;

counter g_binds, g_same, g_skipped;

inline void
add(counter &total, uint64_t value) {
  if (value)
    total.fetch_add(value, std::memory_order_relaxed);
}

inline uint64_t
take(counter &total) {
  return total.exchange(0, std::memory_order_relaxed);
}

std::string
rebindSection(double, uint32_t frames) {
  uint64_t binds = take(g_binds), same = take(g_same), skipped = take(g_skipped);
  if (!frames || !binds)
    return {};
  double f = frames;
  return std::format(
      "skip={} binds={:.1f} same={:.1f} samepct={:.1f} skipped={:.1f}", int(skipRebind()), binds / f, same / f,
      100.0 * double(same) / double(binds), skipped / f
  );
}

SectionRegistrar s_rebind("REBIND", &rebindSection);

} // namespace

bool
skipRebind() {
  static const bool on = [] {
    bool v = env::getEnvVar("DXMT_SKIP_REBIND") != "0";
    Logger::info(
        v ? "Pipeline binds: skip rebind on (DXMT_SKIP_REBIND): a draw's bind of the pipeline its render encoder "
            "already has bound is left out"
          : "Pipeline binds: skip rebind off (DXMT_SKIP_REBIND=0): every pipeline change binds, as upstream"
    );
    return v;
  }();
  return on;
}

void
rebindActed() {
  static std::atomic<bool> logged{false};
  if (logged.load(std::memory_order_relaxed) || logged.exchange(true, std::memory_order_relaxed))
    return;
  Logger::info("Pipeline binds: skipped the first bind of a pipeline already bound in its encoder");
}

void
RebindCounters::publishChunk() {
  if (!on)
    return; // nothing was counted
  add(g_binds, binds);
  add(g_same, same);
  add(g_skipped, skipped);
  *this = {};
}

} // namespace dxmt::gw2
