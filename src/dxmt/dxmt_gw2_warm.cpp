#include "dxmt_gw2_warm.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <format>

// gw2 (W2/W3): see dxmt_gw2_warm.hpp.

namespace dxmt::gw2 {

namespace {

std::string
rawText(const std::string &raw, const std::string &fallback) {
  return raw.empty() ? fallback : raw;
}

uint32_t
parseMax(const std::string &raw, const uint32_t &fallback) {
  if (raw.empty())
    return fallback;
  char *end = nullptr;
  unsigned long v = std::strtoul(raw.c_str(), &end, 10);
  return end && *end == 0 && v > 0 ? uint32_t(std::min<unsigned long>(v, 1u << 20)) : fallback;
}

sw::Switch<std::string> kWarm{"DXMT_WARM", "off", rawText};
sw::Switch<uint32_t> kWarmMax{"DXMT_WARM_MAX", 2048, parseMax};
// the startup line shows the effective mode by name (off for anything not understood)
sw::Effective kWarmLine{"DXMT_WARM", [] { return std::string(warmModeName(warmMode())); }};

std::atomic<uint64_t> g_counters[uint32_t(WarmCounter::Count)];
std::atomic<uint64_t> g_planned_functions{0}, g_planned_pipelines{0}, g_planned_recipes{0}, g_capped{0},
    g_other_target{0}, g_malformed{0};
std::atomic<double> g_read_ms{0};
std::atomic<bool> g_planned_now{false};

std::string
section(double, uint32_t) {
  if (warmMode() == WarmMode::Off)
    return {};
  auto x = [](WarmCounter c) { return g_counters[uint32_t(c)].exchange(0, std::memory_order_relaxed); };
  std::string text = std::format(
      "functions={} pipelines={} skipped={} airmiss={} failed={} nofunctions={} live=variants:{},pipelines:{} "
      "joins={}",
      x(WarmCounter::Functions), x(WarmCounter::Pipelines), x(WarmCounter::Skipped), x(WarmCounter::AirMiss),
      x(WarmCounter::Failed), x(WarmCounter::NoFunctions), x(WarmCounter::LiveVariantHit),
      x(WarmCounter::LivePipelineHit), x(WarmCounter::LiveJoin)
  );
  if (g_planned_now.exchange(false))
    text += std::format(" planned=recipes:{},functions:{},pipelines:{} read={:.1f}ms capped={} othertarget={} "
                        "malformed={}",
                        g_planned_recipes.load(), g_planned_functions.load(), g_planned_pipelines.load(),
                        g_read_ms.load(), g_capped.load(), g_other_target.load(), g_malformed.load());
  return text;
}

SectionRegistrar g_section("WARM", section);

} // namespace

WarmMode
warmMode() {
  static const WarmMode mode = [] {
    auto raw = kWarm();
    return raw == "functions" ? WarmMode::Functions : raw == "all" ? WarmMode::All : WarmMode::Off;
  }();
  return mode;
}

const char *
warmModeName(WarmMode mode) {
  switch (mode) {
  case WarmMode::Functions:
    return "functions";
  case WarmMode::All:
    return "all";
  default:
    return "off";
  }
}

uint32_t
warmMax() {
  static const uint32_t max = kWarmMax();
  return max;
}

void
warmPlanAdd(WarmPlan &plan, const uint8_t *data, size_t length, uint32_t metal_version, WarmMode mode, uint32_t max) {
  compile::PipelineRecipe recipe;
  if (!compile::deserialize(data, length, recipe)) {
    plan.malformed++;
    return;
  }
  if (recipe.metal_version != metal_version) {
    plan.other_target++;
    return;
  }
  if (plan.recipes >= max) {
    plan.capped++;
    return;
  }
  plan.recipes++;
  auto variant = [&](const compile::VariantKey &k, bool pixel) {
    std::string key(reinterpret_cast<const char *>(k.shader.data), sizeof(k.shader.data));
    key.append(reinterpret_cast<const char *>(k.variant.data), sizeof(k.variant.data));
    key.push_back(pixel ? 'p' : 'v');
    auto [it, inserted] = plan.variant_index.try_emplace(key, int(plan.variants.size()));
    if (inserted)
      plan.variants.push_back({k.shader, k.variant, pixel});
    return it->second;
  };
  int vs = variant(*recipe.vs, false);
  int ps = recipe.ps ? variant(*recipe.ps, true) : -1;
  if (mode == WarmMode::All) {
    plan.pipelines.push_back(recipe);
    plan.pipeline_variants.push_back({vs, ps});
  }
}

std::string
warmFunctionName(const WarmVariant &v) {
  // as CreateVariantShader names them: "vs_" / "ps_" + the shader SHA-1's first 8 hex + "_" + digest
  return std::string(v.pixel ? "ps_" : "vs_") + v.shader.string().substr(0, 8) + "_" + v.variant.string();
}

void
warmCount(WarmCounter c, uint64_t n) {
  g_counters[uint32_t(c)].fetch_add(n, std::memory_order_relaxed);
}

void
warmPlanned(const WarmPlan &plan, double read_ms) {
  g_planned_recipes = plan.recipes;
  g_planned_functions = plan.variants.size();
  g_planned_pipelines = plan.pipelines.size();
  g_capped = plan.capped;
  g_other_target = plan.other_target;
  g_malformed = plan.malformed;
  g_read_ms = read_ms;
  g_planned_now = true;
}

} // namespace dxmt::gw2
