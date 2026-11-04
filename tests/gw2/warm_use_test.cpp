// gw2-test: pure
// gw2-build: clang -c -O1 src/util/sha1/sha1.c -o ${OUT}_sha1.o
// gw2-build: clang -c -x objective-c -std=gnu17 -fno-objc-arc -O1 -DDXMT_NO_PRIVATE_API -Isrc/winemetal -Isrc/winemetal/unix src/winemetal/unix/cache.c -o ${OUT}_cache.o
// gw2-build: clang -c -x objective-c -std=gnu17 -fno-objc-arc -O1 -DDXMT_NO_PRIVATE_API -Isrc/winemetal -Isrc/winemetal/unix tests/gw2/warm_use_db.m -o ${OUT}_db.o
// gw2-build: clang++ -std=c++20 -O1 -g -fsanitize=address -Itests/gw2/shim -Itests/gw2 -Isrc/dxmt -Isrc/util -Isrc/winemetal tests/gw2/warm_use_test.cpp src/dxmt/dxmt_gw2_warm.cpp src/dxmt/dxmt_compile_recipe.cpp src/dxmt/dxmt_gw2_stats.cpp src/dxmt/dxmt_gw2_switches.cpp src/util/sha1/sha1_util.cpp ${OUT}_sha1.o ${OUT}_cache.o ${OUT}_db.o -framework Foundation -lsqlite3 -o $OUT
//
// gw2 (#124): warming's plan by use, natively, on a real recipe database written and read through the
// bridge's unix side (src/winemetal/unix/cache.c: CacheWriter_set and CacheEnumerate, call 193), in a
// scratch directory of the user cache dir (removed afterwards). No Metal.
//
// - Migration: a recipes_1.db written before #124 (D, V and R rows, no use rows) plans exactly as the
//   rowid order did: the oldest recipes up to the cap, the rest capped, used=0.
// - Three sessions add use rows to that same file (the recorder's format, dxmt_gw2_recipe_use.hpp): the
//   plan then holds the most recently used recipes (ties: more sessions, then rowid), submitted in
//   first-use order, and the oldest unused ones are left out.
// - A cap above the file: the used recipes first (by first use), then the rest in rowid order.
// - A use row without its recipe is ignored; a malformed one is counted.
#include "dxmt_compile_recipe.hpp"
#include "dxmt_gw2_recipe_use.hpp"
#include "dxmt_gw2_warm.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

using namespace dxmt;

extern "C" {
void warmUseDbRemove(const char *path);
int warmUseDbWrite(const char *path, int count, const void *const *keys, const unsigned *key_lengths,
                   const void *const *values, const unsigned *value_lengths);
void warmUseDbEnumerate(const char *path, uint32_t prefix, uint64_t after, void *buffer, uint64_t capacity,
                        uint64_t *used, uint64_t *last_rowid, uint64_t *need, uint64_t *need_rowid, uint32_t *count,
                        uint32_t *status, uint32_t *error, uint32_t *busy, int *more);
}

static int failures = 0;
#define CHECK(c, ...)                                                                                              \
  do {                                                                                                             \
    if (!(c)) {                                                                                                    \
      failures++;                                                                                                  \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                             \
      std::printf(__VA_ARGS__);                                                                                    \
      std::printf("\n");                                                                                           \
    }                                                                                                              \
  } while (0)

constexpr int kRecipes = 100;
constexpr uint32_t kTarget = 320;

struct Row {
  std::string key;
  std::vector<uint8_t> value;
};

static std::string g_path;
static std::vector<Sha1Digest> g_keys; // recipe i's key (its R row's SHA-1)
static std::map<std::string, int> g_index;

static compile::PipelineRecipe
recipe(int i) {
  WMTRenderPipelineInfo info{};
  WMT::InitializeRenderPipelineInfo(info);
  info.colors[0].pixel_format = WMTPixelFormat(70 + i % 3);
  auto r = compile::recipeFromInfo(info);
  r.metal_version = kTarget;
  uint32_t a = i, b = i + 1000, c = i % 37, d = i % 37 + 2000;
  r.vs = compile::VariantKey{Sha1HashState::compute(&a, 4), Sha1HashState::compute(&b, 4)};
  r.ps = compile::VariantKey{Sha1HashState::compute(&c, 4), Sha1HashState::compute(&d, 4)};
  return r;
}

static void
write(const std::vector<Row> &rows) {
  std::vector<const void *> keys, values;
  std::vector<unsigned> kl, vl;
  for (auto &r : rows) {
    keys.push_back(r.key.data());
    kl.push_back(unsigned(r.key.size()));
    values.push_back(r.value.data());
    vl.push_back(unsigned(r.value.size()));
  }
  int stored = warmUseDbWrite(g_path.c_str(), int(rows.size()), keys.data(), kl.data(), values.data(), vl.data());
  CHECK(stored == int(rows.size()), "stored %d of %zu rows", stored, rows.size());
}

static gw2::WarmEnumerate
enumerator(char prefix) {
  return [prefix](uint64_t after, uint8_t *buffer, uint64_t capacity) {
    gw2::WarmBatch b;
    int more = 0;
    warmUseDbEnumerate(g_path.c_str(), uint8_t(prefix), after, buffer, capacity, &b.used, &b.last_rowid, &b.need,
                       &b.need_rowid, &b.count, &b.status, &b.error, &b.busy, &more);
    b.more = more != 0;
    return b;
  };
}

// The file as #57's recorder left it: per recipe a D row (its shader's DXBC), a V row and the R row.
static void
writeExistingFile() {
  std::vector<Row> rows;
  for (int i = 0; i < kRecipes; i++) {
    auto r = recipe(i);
    std::string d(1, 'D');
    d.append(reinterpret_cast<const char *>(r.vs->shader.data), 20);
    rows.push_back({d, std::vector<uint8_t>(512, uint8_t(i))});
    std::string v(1, 'V');
    v.append(reinterpret_cast<const char *>(r.vs->shader.data), 20);
    v.append(reinterpret_cast<const char *>(r.vs->variant.data), 20);
    v.append("\x40\x01\0\0", 4);
    rows.push_back({v, std::vector<uint8_t>(40, uint8_t(i))});
    gw2::RecipeBytes bytes;
    compile::serialize(r, bytes);
    auto key = compile::recipeKey(r);
    g_keys.push_back(key);
    g_index[std::string(reinterpret_cast<const char *>(key.data), 20)] = i;
    rows.push_back({"R" + std::string(reinterpret_cast<const char *>(key.data), 20), bytes.data});
  }
  write(rows);
}

static void
useSession(uint64_t session, const std::vector<std::pair<int, uint32_t>> &uses) {
  std::vector<Row> rows;
  for (auto [i, first_ms] : uses)
    rows.push_back({gw2::recipeUseKey(g_keys[i], session), gw2::recipeUseValue(first_ms)});
  write(rows);
}

struct Planned {
  gw2::WarmPlan plan;
  gw2::WarmUseTable use;
  std::vector<int> order; // recipe indices, in submission order
};

static Planned
planNow(uint32_t max, bool by_use = true) {
  Planned p;
  std::atomic<bool> stop{false};
  if (by_use) {
    gw2::warmReadUses(p.use, enumerator('U'), stop);
    gw2::warmRead(p.plan, enumerator('R'), kTarget, gw2::WarmMode::All, max, stop, gw2::kWarmRowMax, &p.use);
  } else {
    gw2::warmRead(p.plan, enumerator('R'), kTarget, gw2::WarmMode::All, max, stop);
  }
  for (auto &r : p.plan.pipelines) {
    auto k = compile::recipeKey(r);
    auto it = g_index.find(std::string(reinterpret_cast<const char *>(k.data), 20));
    p.order.push_back(it == g_index.end() ? -1 : it->second);
  }
  return p;
}

static std::vector<int>
range(int from, int to, int step = 1) { // [from, to] inclusive, either direction
  std::vector<int> v;
  for (int i = from; step > 0 ? i <= to : i >= to; i += step)
    v.push_back(i);
  return v;
}

static std::vector<int>
cat(std::initializer_list<std::vector<int>> parts) {
  std::vector<int> v;
  for (auto &p : parts)
    v.insert(v.end(), p.begin(), p.end());
  return v;
}

static std::string
show(const std::vector<int> &v) {
  std::string s;
  for (int i : v)
    s += std::to_string(i) + " ";
  return s;
}

// Optional: WARM_USE_TEST_DB=<path relative to the user cache dir> (a copy of a real recipes_1.db): the
// plan by use and the rowid plan of that file are compared (identical while it has no use rows) and timed.
static void
realFile(const char *path) {
  g_path = path;
  auto t0 = std::chrono::steady_clock::now();
  auto p = planNow(8192);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  auto old = planNow(8192, false);
  bool same = p.use.rows > 0 || (p.plan.recipes == old.plan.recipes && p.plan.capped == old.plan.capped &&
                                 p.plan.pipelines.size() == old.plan.pipelines.size());
  for (size_t i = 0; same && p.use.rows == 0 && i < p.plan.pipelines.size(); i++)
    same = compile::recipeKey(p.plan.pipelines[i]) == compile::recipeKey(old.plan.pipelines[i]);
  CHECK(same && p.plan.read_error == 0 && p.use.read_error == 0, "real file: differs from the rowid plan, or read errors %u/%u (a WAL copy needs its -shm: copy it with journal_mode=delete)", p.plan.read_error, p.use.read_error);
  std::printf("%s real file %s: %llu recipes planned (cap 8192), %llu capped, %zu functions, %llu use rows, "
              "%llu sessions; read and plan %.1f ms; %s\n",
              same ? "ok  " : "FAIL", path, (unsigned long long)p.plan.recipes, (unsigned long long)p.plan.capped,
              p.plan.variants.size(), (unsigned long long)p.use.rows, (unsigned long long)p.plan.sessions, ms,
              p.use.rows ? "has use rows (no rowid comparison)" : "the same plan as rowid order");
}

int
main() {
  if (const char *real = getenv("WARM_USE_TEST_DB")) {
    realFile(real);
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
  }
  char dir[64];
  std::snprintf(dir, sizeof(dir), "dxmt-test-124-%d/recipes_1.db", int(getpid()));
  g_path = dir;
  warmUseDbRemove(g_path.c_str());

  // --- migration: an existing file without use rows plans as the rowid order did
  writeExistingFile();
  {
    auto p = planNow(30);
    auto old = planNow(30, false);
    bool same = p.order == range(0, 29) && old.order == p.order && p.plan.capped == 70 && old.plan.capped == 70 &&
                p.plan.used_planned == 0 && p.plan.used_total == 0 && p.plan.oldest == -1 && p.use.rows == 0 &&
                p.use.read_error == 0 && p.plan.read_error == 0 && p.plan.malformed == 0 && p.plan.by_use;
    CHECK(same, "migration: order %s capped %llu used %llu/%llu oldest %lld use rows %llu errors %u/%u",
          show(p.order).c_str(), (unsigned long long)p.plan.capped, (unsigned long long)p.plan.used_planned,
          (unsigned long long)p.plan.used_total, (long long)p.plan.oldest, (unsigned long long)p.use.rows,
          p.use.read_error, p.plan.read_error);
    std::printf("%s migration: a recipes_1.db without use rows plans the oldest 30 of 100, 70 capped, as rowid "
                "order did\n",
                same ? "ok  " : "FAIL");
  }

  // --- session 1 uses 70..99 (first uses 129..100: 99 first)
  {
    std::vector<std::pair<int, uint32_t>> u;
    for (int i = 70; i <= 99; i++)
      u.push_back({i, uint32_t(100 + (99 - i))});
    useSession(1000, u);
    auto p = planNow(30);
    bool ok = p.order == range(99, 70, -1) && p.plan.capped == 70 && p.plan.used_planned == 30 &&
              p.plan.used_total == 30 && p.plan.sessions == 1 && p.plan.oldest == 0;
    CHECK(ok, "session 1: order %s used %llu/%llu sessions %llu oldest %lld", show(p.order).c_str(),
          (unsigned long long)p.plan.used_planned, (unsigned long long)p.plan.used_total,
          (unsigned long long)p.plan.sessions, (long long)p.plan.oldest);
    std::printf("%s session 1: the plan is the 30 recipes it used (70..99), in first-use order\n",
                ok ? "ok  " : "FAIL");
  }

  // --- session 2 uses 0..9 (first uses 30..39) and 70..79 again (50..59)
  {
    std::vector<std::pair<int, uint32_t>> u;
    for (int i = 0; i <= 9; i++)
      u.push_back({i, uint32_t(30 + i)});
    for (int i = 70; i <= 79; i++)
      u.push_back({i, uint32_t(50 + (i - 70))});
    useSession(2000, u);
    auto p = planNow(30);
    // ranked: 70..79 (session 2, two sessions), 0..9 (session 2), then 80..89 of session 1's 80..99 (rowid)
    auto want = cat({range(0, 9), range(70, 79), range(89, 80, -1)});
    bool ok = p.order == want && p.plan.capped == 70 && p.plan.used_planned == 30 && p.plan.used_total == 40 &&
              p.plan.sessions == 2 && p.plan.oldest == 1;
    CHECK(ok, "session 2: order %s want %s used %llu/%llu sessions %llu oldest %lld", show(p.order).c_str(),
          show(want).c_str(), (unsigned long long)p.plan.used_planned, (unsigned long long)p.plan.used_total,
          (unsigned long long)p.plan.sessions, (long long)p.plan.oldest);
    std::printf("%s session 2: newest first, ties by sessions then rowid; submitted by first use\n",
                ok ? "ok  " : "FAIL");
  }

  // --- session 3 uses 40..59 (first uses 1..20); a use row of a recipe not in the file; a malformed row
  {
    std::vector<std::pair<int, uint32_t>> u;
    for (int i = 40; i <= 59; i++)
      u.push_back({i, uint32_t(1 + (i - 40))});
    useSession(3000, u);
    uint32_t x = 4242;
    write({{gw2::recipeUseKey(Sha1HashState::compute(&x, 4), 3000), gw2::recipeUseValue(0)},
           {"U" + std::string(20, 'z') + std::string(8, '\0'), std::vector<uint8_t>(3, 0)}});
    auto p = planNow(30);
    auto want = cat({range(40, 59), range(70, 79)});
    bool ok = p.order == want && p.plan.capped == 70 && p.plan.used_planned == 30 && p.plan.used_total == 60 &&
              p.plan.sessions == 3 && p.plan.oldest == 1 && p.use.malformed == 1 && p.use.rows == 71;
    CHECK(ok, "session 3: order %s want %s used %llu/%llu sessions %llu oldest %lld malformed %llu rows %llu",
          show(p.order).c_str(), show(want).c_str(), (unsigned long long)p.plan.used_planned,
          (unsigned long long)p.plan.used_total, (unsigned long long)p.plan.sessions, (long long)p.plan.oldest,
          (unsigned long long)p.use.malformed, (unsigned long long)p.use.rows);
    std::printf("%s session 3: the oldest unused recipes (10..39, 60..69) and session 1's alone are left out; "
                "an orphan use row is ignored, a malformed one counted\n",
                ok ? "ok  " : "FAIL");

    // a cap above the file: every recipe, the used ones first (by first use), the rest by rowid
    auto all = planNow(200);
    std::vector<int> used_order(all.order.begin(), all.order.begin() + 60);
    std::vector<int> rest(all.order.begin() + 60, all.order.end());
    auto want_used = cat({range(40, 59), range(0, 9), range(70, 79), range(99, 80, -1)});
    auto want_rest = cat({range(10, 39), range(60, 69)});
    bool full = all.order.size() == 100 && all.plan.capped == 0 && used_order == want_used && rest == want_rest;
    CHECK(full, "cap 200: order %s", show(all.order).c_str());
    std::printf("%s cap above the file: 60 used recipes by first use, then the 40 unused by rowid\n",
                full ? "ok  " : "FAIL");
  }

  warmUseDbRemove(g_path.c_str());
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
