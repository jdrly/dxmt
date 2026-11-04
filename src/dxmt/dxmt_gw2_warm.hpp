#pragma once

// gw2 (W2/W3, #57's plan, architecture council §1): shader warming from the last sessions' recipes.
//
// DXMT_WARM=functions (W2) or all (W3), off by default. At device creation a warm thread reads the
// recorded recipes (#57's recipes_1.db, in first-use order, through bridge call 193) and submits
// speculative compile jobs, up to DXMT_WARM_MAX recipes:
// - functions: each recipe's vertex and pixel variants: the AIR from the shader cache (never a
//   translation: a variant without cached AIR is left to its first use) made into an MTLFunction;
// - all: also each recipe's pipeline, built through the pipeline builder (C3) from the warmed
//   functions.
// Results go into the compile service's shared tables (C4), keyed by stable key, which the live
// variant and pipeline jobs consult first: one build per key per process, whoever comes first, and a
// live job that finds a warm job building its key parks on it (a demand promotes it). Speculative
// jobs run one at a time and never while a demand waits (C2). Warming needs the declared policy, so
// it turns it on.
//
// This file: the switches, the plan (pure: tests/gw2/warm_test.cpp) and the WARM section's counters.

#include "dxmt_compile_recipe.hpp"
#include "sha1/sha1_util.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxmt::gw2 {

enum class WarmMode : uint8_t { Off, Functions, All };

// DXMT_WARM: "functions", "all"; anything else (unset) is off. Parsed once per process.
WarmMode warmMode();
const char *warmModeName(WarmMode mode);
// DXMT_WARM_MAX: recipes planned at most (default 8192 since #124). Parsed once per process.
uint32_t warmMax();
// gw2 (#124) DXMT_WARM_ORDER: "use" (the default) plans the recipes used most recently and most often
// (the use rows); "rowid" restores the plan of #88, the oldest recipes first. Parsed once per process.
bool warmOrderByUse();

struct WarmVariant {
  Sha1Digest shader;
  Sha1Digest variant;
  bool pixel; // the function's name prefix: ps_ (else vs_)
};

struct WarmPlan {
  std::vector<WarmVariant> variants;              // each once, in first-use order
  std::vector<compile::PipelineRecipe> pipelines; // W3 only, in first-use order
  std::vector<std::pair<int, int>> pipeline_variants; // per pipeline: indices into variants (ps: -1 if none)
  uint64_t recipes = 0, other_target = 0, malformed = 0, capped = 0;
  std::unordered_map<std::string, int> variant_index; // shader + variant + kind -> index
  // gw2 (#114): how the read went (warmRead)
  uint32_t read_error = 0;   // SQLite's code when a batch failed (the read stopped there), else 0
  uint64_t busy = 0;         // SQLITE_BUSY retries the bridge made
  uint64_t oversized = 0;    // rows larger than kWarmRowMax, skipped
  uint64_t buffer_bytes = 0; // the read buffer's final size (grown for a row larger than 1 MiB)
  // gw2 (#124): with use data (warmRead's `use`)
  bool by_use = false;
  uint64_t used_planned = 0, used_total = 0; // planned recipes with use rows / every recipe with them
  uint64_t sessions = 0;                     // distinct sessions in the use rows
  uint32_t use_error = 0;                    // the use rows' read (as read_error); the plan goes on without them
  int64_t oldest = -1; // the planned recipes' oldest lastSession, in sessions before the newest (-1: none)
};

// gw2 (#124): what the use rows say about each recipe (dxmt_gw2_recipe_use.hpp)
struct WarmUse {
  uint64_t last_session = 0; // the newest session that used it (unix ms of its start)
  uint32_t sessions = 0;     // sessions that used it
  uint32_t first_ms = UINT32_MAX; // the least first use, ms from a session's start
};
struct WarmUseTable {
  std::unordered_map<std::string, WarmUse> recipes; // by the recipe's SHA-1 (20 bytes)
  std::vector<uint64_t> sessions;                   // distinct sessions, newest first (after warmReadUses)
  uint64_t rows = 0, malformed = 0;
  uint32_t read_error = 0; // as WarmPlan's
};

// gw2 (#114): one batch of bridge call 193 (WMTCacheEnumerateResult's fields, without winemetal.h)
struct WarmBatch {
  uint64_t used = 0, last_rowid = 0, need = 0, need_rowid = 0;
  uint32_t count = 0, status = 0, error = 0, busy = 0;
  bool more = false;
};
using WarmEnumerate = std::function<WarmBatch(uint64_t after_rowid, uint8_t *buffer, uint64_t capacity)>;
constexpr uint64_t kWarmRowMax = 16ull << 20; // a larger row is skipped (a recipe is about 420 B)

// gw2 (#114): the warm thread's read: batches of recipes in rowid order into `plan` until the end or
// `stop`. Past `max` recipes it still reads to the end, so plan.capped counts every recipe over the cap
// (before: only the rest of the batch that reached it). A row too large for the buffer grows it (up to kWarmRowMax) or is skipped;
// a failed batch (status 2) ends the read with its code in plan.read_error. Nothing ends it silently.
//
// gw2 (#124): with `use`, the plan is by use instead of rowid. Every recipe of the target is read; they
// are ranked by lastSession (newest first), then sessionsUsed (most first), then rowid; the first `max`
// are planned, and submitted in firstUseMs order (the character select first), recipes without use rows
// (recorded before #124, or never used since) after them in rowid order.
void warmRead(WarmPlan &plan, const WarmEnumerate &enumerate, uint32_t metal_version, WarmMode mode, uint32_t max,
              const std::atomic<bool> &stop, uint64_t row_max = kWarmRowMax, const WarmUseTable *use = nullptr);

// gw2 (#124): reads the use rows ('U') into `table`, then sorts its sessions newest first.
void warmReadUses(WarmUseTable &table, const WarmEnumerate &enumerate, const std::atomic<bool> &stop,
                  uint64_t row_max = kWarmRowMax);
// gw2 (#124): adds one use row (a 'U' row's key and value); malformed rows are counted and skipped.
void warmUseAdd(WarmUseTable &table, const uint8_t *key, size_t key_length, const uint8_t *value,
                size_t value_length);

// Adds one recorded recipe (an R row's value). Recipes for another Metal target are skipped, and past
// `max` recipes everything is counted as capped.
void warmPlanAdd(WarmPlan &plan, const uint8_t *data, size_t length, uint32_t metal_version, WarmMode mode,
                 uint32_t max);

// The function name DXMT gives the variant (GeneralShaderCompileTask's func_name).
std::string warmFunctionName(const WarmVariant &v);

// WARM section counters.
enum class WarmCounter : uint32_t {
  Functions,     // functions the warm jobs made
  Pipelines,     // pipelines the warm jobs built
  Skipped,       // warm jobs whose key was already done or being built
  AirMiss,       // variants without cached AIR (left to their first use)
  Failed,        // warm builds that failed (left to their first use)
  NoFunctions,   // warm pipelines whose functions weren't warm (left to their first use)
  LiveVariantHit,  // live variant jobs that found their function done
  LivePipelineHit, // live pipeline jobs that found their pipeline done
  LiveJoin,        // live jobs that parked on a warm job building their key
  Count
};
void warmCount(WarmCounter c, uint64_t n = 1);
// The warm thread's read: recipes planned, functions and pipelines queued, and how long it took.
void warmPlanned(const WarmPlan &plan, double read_ms);

} // namespace dxmt::gw2
