#include "dxmt_gw2_warm.hpp"
#include "dxmt_gw2_compile.hpp"
#include "dxmt_gw2_recipe_use.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <format>
#include <functional>
#include <vector>

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

// gw2 (#159): 1 to 8, anything else the default
uint32_t
parseJobs(const std::string &raw, const uint32_t &fallback) {
  if (raw.empty())
    return fallback;
  char *end = nullptr;
  unsigned long v = std::strtoul(raw.c_str(), &end, 10);
  return end && *end == 0 && v >= 1 && v <= 8 ? uint32_t(v) : fallback;
}

sw::Switch<std::string> kWarm{"DXMT_WARM", "off", rawText};
// gw2 (#124): 8192 (about two sessions of GW2's 2,700-4,800 pipelines); #88's was 2048
sw::Switch<uint32_t> kWarmMax{"DXMT_WARM_MAX", 8192, parseMax};
// gw2 (#124): "use" (default) or "rowid" (#88's plan)
sw::Switch<std::string> kWarmOrder{"DXMT_WARM_ORDER", "use", rawText};
// gw2 (#159): speculative jobs at once while the warm builds are slow; 1 (default): one at a time
sw::Switch<uint32_t> kWarmJobs{"DXMT_WARM_JOBS", 1, parseJobs};
// the startup line shows the effective mode by name (off for anything not understood)
sw::Effective kWarmLine{"DXMT_WARM", [] { return std::string(warmModeName(warmMode())); }};
// gw2 (#124): the order in effect (anything but "rowid" plans by use)
sw::Effective kWarmOrderLine{"DXMT_WARM_ORDER", [] { return std::string(warmOrderByUse() ? "use" : "rowid"); }};

std::atomic<uint64_t> g_counters[uint32_t(WarmCounter::Count)];
std::atomic<uint64_t> g_planned_functions{0}, g_planned_pipelines{0}, g_planned_recipes{0}, g_capped{0},
    g_other_target{0}, g_malformed{0};
std::atomic<uint64_t> g_read_error{0}, g_busy{0}, g_oversized{0}, g_buffer_bytes{0}; // gw2 (#114)
std::atomic<double> g_read_ms{0};
// gw2 (#124): the plan by use
std::atomic<bool> g_by_use{false};
std::atomic<uint64_t> g_used_planned{0}, g_used_total{0}, g_newest{0}, g_use_error{0};
constinit std::atomic<WarmHitUse> g_hit_use{nullptr};
std::atomic<int64_t> g_oldest{-1};
std::atomic<bool> g_planned_now{false};
// gw2 (#159): speculative jobs running now, and the most at once in the window
std::atomic<uint32_t> g_jobs_now{0}, g_jobs_peak{0};

std::string
section(double, uint32_t) {
  if (warmMode() == WarmMode::Off)
    return {};
  auto x = [](WarmCounter c) { return g_counters[uint32_t(c)].exchange(0, std::memory_order_relaxed); };
  std::string text = std::format(
      "functions={} pipelines={} skipped={} airmiss={} failed={} nofunctions={} live=variants:{},pipelines:{} "
      "joins={} jobs={}/{}",
      x(WarmCounter::Functions), x(WarmCounter::Pipelines), x(WarmCounter::Skipped), x(WarmCounter::AirMiss),
      x(WarmCounter::Failed), x(WarmCounter::NoFunctions), x(WarmCounter::LiveVariantHit),
      x(WarmCounter::LivePipelineHit), x(WarmCounter::LiveJoin), g_jobs_peak.exchange(0, std::memory_order_relaxed),
      warmJobs()
  );
  // the next window starts from the jobs still running (a change after the reset raises it itself)
  warmJobsRunning(g_jobs_now.load(std::memory_order_relaxed));
  if (g_planned_now.exchange(false))
    text += std::format(" planned=recipes:{},functions:{},pipelines:{} read={:.1f}ms capped={} othertarget={} "
                        "malformed={}",
                        g_planned_recipes.load(), g_planned_functions.load(), g_planned_pipelines.load(),
                        g_read_ms.load(), g_capped.load(), g_other_target.load(), g_malformed.load()) +
            // gw2 (#114): how the read went; readerror= is SQLite's code (5: SQLITE_BUSY), 0 when read
            std::format(" readerror={} busy={} oversized={} buffer={}KB", g_read_error.load(), g_busy.load(),
                        g_oversized.load(), g_buffer_bytes.load() >> 10) +
            // gw2 (#124): the order, and how much of the plan the use rows chose
            (g_by_use.load() ? std::format(" order=use used={}/{} newest={} oldest={} useerror={}",
                                           g_used_planned.load(), g_used_total.load(), g_newest.load(),
                                           g_oldest.load() < 0 ? std::string("-") : std::to_string(g_oldest.load()),
                                           g_use_error.load())
                             : std::string(" order=rowid"));
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

bool
warmOrderByUse() {
  static const bool by_use = kWarmOrder() != "rowid";
  return by_use;
}

uint32_t
warmJobs() {
  static const uint32_t jobs = kWarmJobs();
  return jobs;
}

void
warmJobsRunning(uint32_t running) {
  g_jobs_now.store(running, std::memory_order_relaxed);
  uint32_t peak = g_jobs_peak.load(std::memory_order_relaxed);
  while (running > peak && !g_jobs_peak.compare_exchange_weak(peak, running, std::memory_order_relaxed))
    ;
}

uint32_t
warmMax() {
  static const uint32_t max = kWarmMax();
  return max;
}

namespace {

// the plan's part of a recipe: its variants once each, and (W3) the pipeline
void
planRecipe(WarmPlan &plan, const compile::PipelineRecipe &recipe, WarmMode mode) {
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

struct ReadStatus {
  uint32_t read_error = 0;
  uint64_t busy = 0, oversized = 0, buffer_bytes = 0;
};

// gw2 (#114): every row of one prefix, in rowid order, until the end or `stop`. A row too large for
// the buffer grows it (up to `row_max`) or is skipped; a failed batch ends the read with its code.
template <typename Row>
void
readRows(const WarmEnumerate &enumerate, const std::atomic<bool> &stop, uint64_t row_max, ReadStatus &status,
         Row &&row) {
  std::vector<uint8_t> buffer(1 << 20);
  uint64_t after = 0;
  bool more = true;
  // read to the end even past the cap: every recipe over DXMT_WARM_MAX is counted as capped, not
  // only the rest of the batch that reached it (the read costs about 5 ms per 1 MiB batch)
  while (more && !stop.load()) {
    WarmBatch b = enumerate(after, buffer.data(), buffer.size());
    status.busy += b.busy;
    for (uint64_t p = 0, i = 0; i < b.count && p + 8 <= b.used; i++) {
      auto u32 = [&](uint64_t at) {
        return uint32_t(buffer[at]) | uint32_t(buffer[at + 1]) << 8 | uint32_t(buffer[at + 2]) << 16 |
               uint32_t(buffer[at + 3]) << 24;
      };
      uint32_t key_length = u32(p);
      uint32_t value_length = u32(p + 4 + key_length);
      row(buffer.data() + p + 4, key_length, buffer.data() + p + 8 + key_length, value_length);
      p += 8ull + key_length + value_length;
    }
    if (b.status == 2) {
      status.read_error = b.error ? b.error : 1; // the rows before it were read; the rest is lost
      break;
    }
    more = b.more;
    if (b.count == 0 && more) {
      // the next row alone doesn't fit: grow the buffer for it, or skip a row no recipe could be
      if (b.need > buffer.size() && b.need <= row_max) {
        buffer.resize(b.need);
        continue;
      }
      if (b.need > row_max && b.need_rowid > after) {
        status.oversized++;
        after = b.need_rowid;
        continue;
      }
      status.read_error = 1; // an answer that can't be stepped past: stop, don't loop (SQLITE_ERROR)
      break;
    }
    if (b.count == 0)
      break;
    after = b.last_rowid;
  }
  status.buffer_bytes = buffer.size();
}

} // namespace

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
  planRecipe(plan, recipe, mode);
}

void
warmRead(WarmPlan &plan, const WarmEnumerate &enumerate, uint32_t metal_version, WarmMode mode, uint32_t max,
         const std::atomic<bool> &stop, uint64_t row_max, const WarmUseTable *use) {
  ReadStatus status;
  if (!use) {
    // rowid order (#88; DXMT_WARM_ORDER=rowid): the oldest recipes first, streamed
    readRows(enumerate, stop, row_max, status, [&](const uint8_t *, uint32_t, const uint8_t *value, uint32_t length) {
      warmPlanAdd(plan, value, length, metal_version, mode, max);
    });
  } else {
    // gw2 (#124): every recipe of the target, then the plan by use
    struct Candidate {
      compile::PipelineRecipe recipe;
      const RecipeUse *use;
      uint64_t index; // rowid order
    };
    std::vector<Candidate> all;
    readRows(enumerate, stop, row_max, status,
             [&](const uint8_t *key, uint32_t key_length, const uint8_t *value, uint32_t length) {
               compile::PipelineRecipe recipe;
               if (!compile::deserialize(value, length, recipe)) {
                 plan.malformed++;
                 return;
               }
               if (recipe.metal_version != metal_version) {
                 plan.other_target++;
                 return;
               }
               const RecipeUse *u = nullptr;
               if (key_length == 21) {
                 auto it = use->recipes.find(std::string(reinterpret_cast<const char *>(key) + 1, 20));
                 if (it != use->recipes.end())
                   u = &it->second;
               }
               all.push_back({std::move(recipe), u, uint64_t(all.size())});
             });
    std::vector<Candidate *> ranked;
    ranked.reserve(all.size());
    for (auto &c : all) {
      ranked.push_back(&c);
      plan.used_total += c.use != nullptr;
    }
    // newest lastSession, then most sessions, then rowid (a recipe without use rows: session 0)
    std::stable_sort(ranked.begin(), ranked.end(), [](const Candidate *a, const Candidate *b) {
      uint64_t la = a->use ? a->use->last_session : 0, lb = b->use ? b->use->last_session : 0;
      if (la != lb)
        return la > lb;
      uint32_t sa = a->use ? a->use->sessions : 0, sb = b->use ? b->use->sessions : 0;
      return sa > sb;
    });
    size_t planned = std::min<size_t>(ranked.size(), max);
    plan.capped += ranked.size() - planned;
    ranked.resize(planned);
    // submission: first use (the character select first), then the recipes without use rows by rowid
    std::sort(ranked.begin(), ranked.end(), [](const Candidate *a, const Candidate *b) {
      uint32_t fa = a->use ? a->use->first_ms : UINT32_MAX, fb = b->use ? b->use->first_ms : UINT32_MAX;
      bool ua = a->use != nullptr, ub = b->use != nullptr;
      if (ua != ub)
        return ua;
      if (fa != fb)
        return fa < fb;
      return a->index < b->index;
    });
    uint64_t oldest_session = UINT64_MAX;
    for (auto *c : ranked) {
      planRecipe(plan, c->recipe, mode);
      if (c->use) {
        plan.used_planned++;
        oldest_session = std::min<uint64_t>(oldest_session, c->use->last_session);
      }
    }
    plan.by_use = true;
    plan.newest = use->newest;
    plan.use_error = use->read_error;
    if (oldest_session != UINT64_MAX)
      plan.oldest = int64_t(use->newest) - int64_t(oldest_session);
  }
  plan.read_error = status.read_error;
  plan.busy += status.busy;
  plan.oversized += status.oversized;
  plan.buffer_bytes = status.buffer_bytes;
}

void
warmUseAdd(WarmUseTable &table, const uint8_t *key, size_t key_length, const uint8_t *value, size_t value_length) {
  std::string id;
  RecipeUse u;
  if (!parseRecipeUse(key, key_length, value, value_length, id, u)) {
    table.malformed++;
    return;
  }
  table.rows++;
  table.newest = std::max(table.newest, u.last_session);
  table.recipes[id] = u;
}

void
warmReadUses(WarmUseTable &table, const WarmEnumerate &enumerate, const std::atomic<bool> &stop, uint64_t row_max) {
  ReadStatus status;
  readRows(enumerate, stop, row_max, status,
           [&](const uint8_t *key, uint32_t key_length, const uint8_t *value, uint32_t length) {
             warmUseAdd(table, key, key_length, value, length);
           });
  table.read_error = status.read_error;
}

uint32_t
warmReadRows(const WarmEnumerate &enumerate, const std::atomic<bool> &stop,
             const std::function<void(const uint8_t *, uint32_t, const uint8_t *, uint32_t)> &row, uint64_t row_max) {
  ReadStatus status;
  readRows(enumerate, stop, row_max, status, row);
  return status.read_error;
}

void
setWarmHitUse(WarmHitUse hook) {
  g_hit_use.store(hook, std::memory_order_release);
}

void
warmPipelineHit(const Sha1Digest &recipe_key) {
  warmCount(WarmCounter::LivePipelineHit);
  if (auto hook = g_hit_use.load(std::memory_order_acquire))
    hook(recipe_key);
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
  g_read_error = plan.read_error;
  g_busy = plan.busy;
  g_oversized = plan.oversized;
  g_buffer_bytes = plan.buffer_bytes;
  g_by_use = plan.by_use;
  g_used_planned = plan.used_planned;
  g_used_total = plan.used_total;
  g_newest = plan.newest;
  g_oldest = plan.oldest;
  g_use_error = plan.use_error;
  g_planned_now = true;
}

} // namespace dxmt::gw2
