#include "dxmt_gw2_setters.hpp"
#include <algorithm>
#include <atomic>
#include <format>

namespace dxmt::gw2 {

namespace {
std::atomic<uint64_t> g_setters[kSetterCount];
} // namespace

void
SetterCounters::publish() {
  for (unsigned i = 0; i < kSetterCount; i++) {
    if (n[i])
      g_setters[i].fetch_add(n[i], std::memory_order_relaxed);
    n[i] = 0;
  }
}

std::string
setterFields(uint32_t frames, bool &any) {
  double c[kSetterCount];
  any = false;
  for (unsigned i = 0; i < kSetterCount; i++) {
    uint64_t v = g_setters[i].exchange(0, std::memory_order_relaxed);
    any |= v != 0;
    c[i] = double(v);
  }
  double n = std::max(frames, 1u);
  auto at = [&](Setter s) { return c[(unsigned)s] / n; };
  return std::format(
      "samefactor={:.1f} samedss={:.1f} nullrs={:.1f} soset={:.1f} blendcmd={:.1f} dsscmd={:.1f} rscmd={:.1f}",
      at(Setter::SameFactor), at(Setter::SameDss), at(Setter::NullRs), at(Setter::SoSet), at(Setter::BlendCmd),
      at(Setter::DssCmd), at(Setter::RsCmd)
  );
}

} // namespace dxmt::gw2
