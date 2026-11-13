// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/switches_test.cpp src/dxmt/dxmt_gw2_switches.cpp -o $OUT
//
// gw2 (#60): host-side test of the switch registry (src/dxmt/dxmt_gw2_switches.*) and a
// characterization of the switches R1 migrated, against the reads they replaced (copied below from
// gw2-on-mac 0034ffa, the "legacy oracles"):
// - each grammar over every spelling worth trying;
// - each caller's read sequence (when the environment is sampled, what is cached where), with the
//   environment changing between reads;
// - the generated startup line against today's format, byte for byte.
// gw2 (#226): and DXMT_MTL4_LAYER_SET's two values, with its declaration's name, fallback and grammar
// (src/dxmt/dxmt_gw2_layer_set.hpp).
// Not part of the DLL build; run it on the Mac from the repository root (or through
// tests/gw2/run.zsh):
//
//   clang++ -std=c++20 -Itests/gw2/shim -Isrc/dxmt tests/gw2/switches_test.cpp \
//     src/dxmt/dxmt_gw2_switches.cpp -o /tmp/switches_test && /tmp/switches_test

#include "dxmt_gw2_switches.hpp"
#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_layer_set.hpp"
#include <climits>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace dxmt;

static int failures = 0;
static int checks = 0;
#define CHECK(...)                                                                                 \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(__VA_ARGS__)) {                                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)
#define CHECK_EQ(a, b, what)                                                                       \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!((a) == (b))) {                                                                           \
      std::printf("FAIL %s:%d: %s: %s != %s (%s)\n", __FILE__, __LINE__, #a, std::format("{}", a).c_str(), \
                  std::format("{}", b).c_str(), std::string(what).c_str());                        \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// A process environment both the legacy oracles and the migrated switches read; tests change it
// between reads. Unset names read as "", as env::getEnvVar returns them.
struct Env {
  std::map<std::string, std::string> vars;
  int reads = 0;
  std::string
  get(const char *name) {
    reads++;
    auto it = vars.find(name);
    return it == vars.end() ? std::string() : it->second;
  }
  sw::Source
  source() {
    return [this](const char *name) { return get(name); };
  }
};

// Every spelling worth trying: unset/empty, today's accepted values, near misses, malformed text,
// NaN, signs, whitespace, hex/octal/exponent forms, and 32/64-bit range edges.
static const std::vector<std::optional<std::string>> kInputs = {
    std::nullopt, "", "0", "1", "2", "3", "4", "00", "01", "10", " 1", "1 ", "\t1", "+1", "-0", "-1",
    "true", "false", "on", "off", "yes", "no", "TRUE", "abc", "1abc", "nan", "NaN", "inf", "-inf",
    "1e3", "0x10", "0x20", "040", "32", "64", "128", "256", "16", "31", "33", " 64", "64 ", "064",
    "64abc", "-64", "+128", "128.0", "4294967295", "4294967296", "4294967328", "18446744073709551615",
    "18446744073709551616", "99999999999999999999999",
};

static std::string
show(const std::optional<std::string> &v) {
  return v ? "\"" + *v + "\"" : std::string("unset");
}

// --- legacy oracles: the reads R1 replaced, verbatim apart from reading `env` instead of getEnvVar
// and a struct member instead of a function-local static

constexpr uint32_t kCommandChunkCount = 32;

struct Queue {
  uint32_t chunks = 0, metal_buffers = 0;
  bool warned = false;
  std::string warning, marker;
};

struct Legacy {
  explicit Legacy(Env &e) : env(e) {}
  Env &env;
  // dxmt_gw2_stats.cpp statsEnabled(): `static const bool enabled = getEnvVar("DXMT_STATS") != "0";`
  std::optional<bool> stats_static;
  bool
  statsEnabled() {
    if (!stats_static)
      stats_static = env.get("DXMT_STATS") != "0";
    return *stats_static;
  }
  // d3d11_swapchain.cpp: `bool stats_enabled_ = getEnvVar("DXMT_STATS") != "0";` per swapchain
  bool
  swapchainStats() {
    return env.get("DXMT_STATS") != "0";
  }
  // d3d11_resource_helper.cpp: two function statics
  std::optional<bool> stencil_static, depth_static;
  bool
  stencilPfv() {
    if (!stencil_static)
      stencil_static = env.get("DXMT_STENCIL_PFV") == "1";
    return *stencil_static;
  }
  bool
  depthPfv() {
    if (!depth_static)
      depth_static = env.get("DXMT_DEPTH_PFV") == "1";
    return *depth_static;
  }
  // d3d11_swapchain.cpp RaiseRenderThreadPriority: a function static
  std::optional<uint32_t> rtp_static;
  uint32_t
  renderThreadPriority() {
    if (!rtp_static)
      rtp_static = [&] {
        auto v = env.get("DXMT_RENDER_THREAD_PRIORITY");
        return v.empty() ? 0u : uint32_t(std::strtoul(v.c_str(), nullptr, 10));
      }();
    return *rtp_static;
  }
  // dxmt_command_queue.cpp, per queue: ChunkCountFromEnv, MetalBufferCount, the CommandQueue line
  Queue
  makeQueue() {
    Queue q;
    q.chunks = [&] {
      auto v = env.get("DXMT_CHUNKS");
      if (v.empty())
        return kCommandChunkCount;
      auto n = std::strtoul(v.c_str(), nullptr, 10);
      if (n == 32 || n == 64 || n == 128)
        return uint32_t(n);
      q.warned = true;
      q.warning = "DXMT_CHUNKS=" + v + " is not 32, 64 or 128; using 32";
      return kCommandChunkCount;
    }();
    q.metal_buffers = env.get("DXMT_CHUNKS").empty() ? kCommandChunkCount : q.chunks + 16;
    q.marker = env.get("DXMT_CHUNKS").empty() ? " (default)" : " (DXMT_CHUNKS)";
    return q;
  }
  // d3d11_swapchain.cpp CreateSwapChain and dxmt_gw2_metal4.cpp metal4AttachQueue, each call
  bool
  metalfxSpatialSwapchain() {
    return env.get("DXMT_METALFX_SPATIAL_SWAPCHAIN") == "1";
  }
  // LogStartup's own reads: the PFVs and the priority, read when the line is logged
  std::string
  linePfv(const char *name) {
    return std::format("{}", int(env.get(name) == "1"));
  }
  std::string
  lineRtp() {
    return std::format("{}", std::strtoul(env.get("DXMT_RENDER_THREAD_PRIORITY").c_str(), nullptr, 10));
  }
};

// --- the migrated code: the declarations R1 put in the modules (same names, fallbacks, parsers and
// listing) on an isolated registry, and the callers as R1 wrote them

struct Migrated {
  sw::Registry registry;
  sw::Switch<bool> stats;
  sw::Effective stats_line;
  sw::Switch<bool> stencil_pfv, depth_pfv;
  sw::Switch<uint32_t> render_thread_priority, chunks;
  sw::Switch<bool> metalfx_spatial_swapchain;

  explicit Migrated(Env &env)
      : registry(env.source()), stats{"DXMT_STATS", true, sw::legacy::notZero, sw::Line::Listed, registry},
        stats_line{"DXMT_STATS", [this] { return sw::formatValue(statsEnabled()); }, registry},
        stencil_pfv{"DXMT_STENCIL_PFV", false, sw::legacy::exactlyOne, sw::Line::Listed, registry},
        depth_pfv{"DXMT_DEPTH_PFV", false, sw::legacy::exactlyOne, sw::Line::Listed, registry},
        render_thread_priority{"DXMT_RENDER_THREAD_PRIORITY", 0, sw::legacy::strtoul32, sw::Line::Listed, registry},
        chunks{"DXMT_CHUNKS", kCommandChunkCount, sw::legacy::ulongOneOf<32, 64, 128>, sw::Line::Listed, registry},
        metalfx_spatial_swapchain{
            "DXMT_METALFX_SPATIAL_SWAPCHAIN", false, sw::legacy::exactlyOne, sw::Line::Unlisted, registry
        } {}

  // dxmt_gw2_stats.cpp
  std::optional<bool> stats_static;
  bool
  statsEnabled() {
    if (!stats_static)
      stats_static = stats();
    return *stats_static;
  }
  bool
  statsReadNow() {
    return stats();
  }
  // d3d11_resource_helper.cpp
  std::optional<bool> stencil_static, depth_static;
  bool
  stencilPfv() {
    if (!stencil_static)
      stencil_static = stencil_pfv();
    return *stencil_static;
  }
  bool
  depthPfv() {
    if (!depth_static)
      depth_static = depth_pfv();
    return *depth_static;
  }
  // d3d11_swapchain.cpp
  std::optional<uint32_t> rtp_static;
  uint32_t
  renderThreadPriority() {
    if (!rtp_static)
      rtp_static = render_thread_priority();
    return *rtp_static;
  }
  // dxmt_command_queue.cpp
  Queue
  makeQueue() {
    Queue q;
    q.chunks = [&] {
      auto v = chunks.requested();
      if (!v.empty() && !sw::legacy::ulongIsOneOf<32, 64, 128>(v)) {
        q.warned = true;
        q.warning = "DXMT_CHUNKS=" + v + " is not 32, 64 or 128; using 32";
      }
      return chunks.parse(v);
    }();
    q.metal_buffers = chunks.requested().empty() ? kCommandChunkCount : q.chunks + 16;
    q.marker = chunks.requested().empty() ? " (default)" : " (DXMT_CHUNKS)";
    return q;
  }
  // dxmt_gw2_metal4.cpp metalfxSpatialSwapchain()
  bool
  metalfxSpatialSwapchain() {
    return metalfx_spatial_swapchain();
  }
};

static void
checkQueue(const Queue &a, const Queue &b, const std::string &what) {
  CHECK_EQ(a.chunks, b.chunks, what + ": chunks");
  CHECK_EQ(a.metal_buffers, b.metal_buffers, what + ": metal buffers");
  CHECK_EQ(a.warned, b.warned, what + ": warned");
  CHECK_EQ(a.warning, b.warning, what + ": warning");
  CHECK_EQ(a.marker, b.marker, what + ": marker");
}

// --- grammars: every spelling, a fresh process each

static void
testMigratedGrammars() {
  const char *names[] = {"DXMT_STATS",  "DXMT_STENCIL_PFV", "DXMT_DEPTH_PFV", "DXMT_RENDER_THREAD_PRIORITY",
                         "DXMT_CHUNKS", "DXMT_METALFX_SPATIAL_SWAPCHAIN"};
  for (auto &input : kInputs) {
    Env env, env2;
    if (input)
      for (auto *n : names)
        env.vars[n] = env2.vars[n] = *input;
    Legacy old(env);
    Migrated now(env2);
    std::string what = show(input);

    CHECK_EQ(now.statsEnabled(), old.statsEnabled(), "DXMT_STATS " + what);
    CHECK_EQ(now.statsReadNow(), old.swapchainStats(), "DXMT_STATS per swapchain " + what);
    CHECK_EQ(now.stencilPfv(), old.stencilPfv(), "DXMT_STENCIL_PFV " + what);
    CHECK_EQ(now.depthPfv(), old.depthPfv(), "DXMT_DEPTH_PFV " + what);
    CHECK_EQ(now.renderThreadPriority(), old.renderThreadPriority(), "DXMT_RENDER_THREAD_PRIORITY " + what);
    checkQueue(now.makeQueue(), old.makeQueue(), "DXMT_CHUNKS " + what);
    CHECK_EQ(now.metalfxSpatialSwapchain(), old.metalfxSpatialSwapchain(), "DXMT_METALFX_SPATIAL_SWAPCHAIN " + what);

    // the registry's startup values, as LogStartup read them
    CHECK_EQ(*now.registry.effective("DXMT_STENCIL_PFV"), old.linePfv("DXMT_STENCIL_PFV"), "line " + what);
    CHECK_EQ(*now.registry.effective("DXMT_DEPTH_PFV"), old.linePfv("DXMT_DEPTH_PFV"), "line " + what);
    // the old line printed strtoul's unsigned long, the new one the switch's uint32_t: the same on
    // Windows (32-bit long; out-of-range text saturates to 4294967295 in both). Compared where this
    // host's 64-bit long fits in 32 bits.
    if (std::strtoul(input.value_or("").c_str(), nullptr, 10) <= UINT32_MAX)
      CHECK_EQ(*now.registry.effective("DXMT_RENDER_THREAD_PRIORITY"), old.lineRtp(), "line " + what);
    CHECK(!now.registry.effective("DXMT_METALFX_SPATIAL_SWAPCHAIN")); // never on the line
    // every read above sampled the environment as often in both
    CHECK_EQ(env2.reads, env.reads, "source reads " + what);
    CHECK(now.chunks.requested() == input.value_or("")); // the requested value is the raw text
  }
}

// --- read sequences: the environment changes between reads; old and new must sample it at the
// same moments and cache it in the same places

static void
testReadSequences() {
  // queues: each queue reads DXMT_CHUNKS anew (queue A 32, then 64 for queue B, then bad values)
  {
    Env env, env2;
    Legacy old(env);
    Migrated now(env2);
    for (auto v : {"32", "64", "100", "", "128", "abc"}) {
      env.vars["DXMT_CHUNKS"] = env2.vars["DXMT_CHUNKS"] = v;
      int r0 = env.reads, r1 = env2.reads;
      checkQueue(now.makeQueue(), old.makeQueue(), std::string("queue with ") + v);
      CHECK_EQ(env2.reads - r1, env.reads - r0, std::string("source reads per queue, ") + v);
    }
  }
  // STATS: the process value is the first read's; each swapchain reads it again
  {
    Env env, env2;
    Legacy old(env);
    Migrated now(env2);
    env.vars["DXMT_STATS"] = env2.vars["DXMT_STATS"] = "1";
    CHECK(now.statsEnabled() == old.statsEnabled());
    env.vars["DXMT_STATS"] = env2.vars["DXMT_STATS"] = "0";
    CHECK(now.statsEnabled() == old.statsEnabled()); // still the cached 1
    CHECK(now.statsEnabled() == true);
    bool swapchain_now = now.statsReadNow();
    CHECK(swapchain_now == old.swapchainStats()); // a new swapchain sees 0
    CHECK(swapchain_now == false);
    CHECK(env2.reads == env.reads);
    // the startup line shows the process value (it exists only with STATS on): 1, as the literal was
    CHECK(*now.registry.effective("DXMT_STATS") == "1");
  }
  // PFVs and the priority: the callers' statics keep the first value; the startup line reads now
  {
    Env env, env2;
    Legacy old(env);
    Migrated now(env2);
    env.vars["DXMT_STENCIL_PFV"] = env2.vars["DXMT_STENCIL_PFV"] = "1";
    env.vars["DXMT_RENDER_THREAD_PRIORITY"] = env2.vars["DXMT_RENDER_THREAD_PRIORITY"] = "3";
    CHECK(now.stencilPfv() == old.stencilPfv() && now.stencilPfv());
    CHECK(now.renderThreadPriority() == old.renderThreadPriority());
    env.vars["DXMT_STENCIL_PFV"] = env2.vars["DXMT_STENCIL_PFV"] = "0";
    env.vars["DXMT_RENDER_THREAD_PRIORITY"] = env2.vars["DXMT_RENDER_THREAD_PRIORITY"] = "2";
    auto stencil_line = *now.registry.effective("DXMT_STENCIL_PFV");
    CHECK(stencil_line == old.linePfv("DXMT_STENCIL_PFV")); // 0 in both
    CHECK(stencil_line == "0");
    CHECK(*now.registry.effective("DXMT_RENDER_THREAD_PRIORITY") == old.lineRtp()); // 2 in both
    CHECK(now.stencilPfv() == old.stencilPfv() && now.stencilPfv()); // the static still 1
    CHECK(now.renderThreadPriority() == 3 && old.renderThreadPriority() == 3);
    CHECK(env2.reads == env.reads);
  }
  // logging first doesn't initialize a caller's static: the resource path's first read comes later
  {
    Env env, env2;
    Legacy old(env);
    Migrated now(env2);
    env.vars["DXMT_DEPTH_PFV"] = env2.vars["DXMT_DEPTH_PFV"] = "1";
    CHECK(*now.registry.effective("DXMT_DEPTH_PFV") == old.linePfv("DXMT_DEPTH_PFV"));
    CHECK(!now.depth_static);
    env.vars["DXMT_DEPTH_PFV"] = env2.vars["DXMT_DEPTH_PFV"] = "0";
    CHECK(now.depthPfv() == old.depthPfv() && !now.depthPfv());
  }
  // DXMT_METALFX_SPATIAL_SWAPCHAIN: read at every call
  {
    Env env, env2;
    Legacy old(env);
    Migrated now(env2);
    for (auto v : {"1", "0", "1", ""}) {
      env.vars["DXMT_METALFX_SPATIAL_SWAPCHAIN"] = env2.vars["DXMT_METALFX_SPATIAL_SWAPCHAIN"] = v;
      CHECK(now.metalfxSpatialSwapchain() == old.metalfxSpatialSwapchain());
    }
    CHECK(env2.reads == env.reads);
  }
  // declaring reads nothing
  {
    Env env;
    Migrated now(env);
    sw::Switch<bool> extra{"DXMT_EXTRA", false, sw::legacy::exactlyOne, sw::Line::Listed, now.registry};
    CHECK(env.reads == 0);
  }
}

// --- registry rules

static void
testRequestedVsEffective() {
  // a capability outcome: asked for (requested "1", parsed on), but the capability check failed
  Env env;
  env.vars["DXMT_CAP"] = "1";
  sw::Registry reg(env.source());
  sw::Switch<bool> cap{"DXMT_CAP", false, sw::legacy::exactlyOne, sw::Line::Listed, reg};
  bool capability_ok = false;
  sw::provideEffective("DXMT_CAP", [&] { return std::string(!cap() ? "0" : capability_ok ? "1" : "failed"); }, reg);
  CHECK(cap() == true); // the request
  CHECK(cap.requested() == "1");
  CHECK(sw::startupLine(reg, {}) == "DXMT_CAP=failed"); // the effective value
  capability_ok = true;
  CHECK(sw::startupLine(reg, {}) == "DXMT_CAP=1");

  // a provider declared before its switch keeps precedence
  sw::Effective late_line{"DXMT_LATE", [] { return std::string("unsupported"); }, reg};
  sw::Switch<bool> late{"DXMT_LATE", true, sw::legacy::notZero, sw::Line::Listed, reg};
  CHECK(late() == true);
  CHECK(sw::startupLine(reg, {}) == "DXMT_CAP=1 DXMT_LATE=unsupported");

  // a provider with no switch is a listed entry of its own
  sw::provideEffective("DXMT_ONLY", [] { return std::string("x"); }, reg);
  CHECK(sw::startupLine(reg, {}) == "DXMT_CAP=1 DXMT_LATE=unsupported DXMT_ONLY=x");
}

static void
testRegistryRules() {
  Env env;
  sw::Registry reg(env.source());
  CHECK(sw::startupLine(reg, {}).empty());
  CHECK(sw::startupLine(reg, {}, {{"cpus", "8"}}) == "cpus=8");

  sw::Switch<bool> b{"DXMT_B", true, sw::legacy::notZero, sw::Line::Listed, reg};
  sw::Switch<bool> a{"DXMT_A", false, sw::legacy::exactlyOne, sw::Line::Listed, reg};
  sw::Switch<bool> z{"DXMT_Z", false, sw::legacy::exactlyOne, sw::Line::Listed, reg};
  sw::Switch<bool> hidden{"DXMT_HIDDEN", true, sw::legacy::notZero, sw::Line::Unlisted, reg};
  // one declaration per name
  CHECK(!reg.add("DXMT_B", [] { return std::string("dup"); }, true));
  // the caller's fields in its order (a field without a value from the registry), the rest of the
  // registry by name, then the tail; an unlisted switch only where a caller gives its value
  CHECK(sw::startupLine(reg, {{"DXMT_Z", std::nullopt}, {"DXMT_Q", "q r"}, {"DXMT_B", std::nullopt}},
                        {{"cpus", "8"}, {"refresh", "60"}}) ==
        "DXMT_Z=0 DXMT_Q=q r DXMT_B=1 DXMT_A=0 cpus=8 refresh=60");
  // a caller's value replaces the registry's; a name without a value and without an entry is left out
  CHECK(sw::startupLine(reg, {{"DXMT_A", "override"}, {"DXMT_NONE", std::nullopt}, {"DXMT_HIDDEN", std::nullopt}}) ==
        "DXMT_A=override DXMT_B=1 DXMT_Z=0");
  // a name in the tail isn't repeated in the middle
  CHECK(sw::startupLine(reg, {}, {{"DXMT_B", std::nullopt}}) == "DXMT_A=0 DXMT_Z=0 DXMT_B=1");
  CHECK(hidden() == true);

  // formatting: bools 0/1, integers and doubles as std::format's {} writes them
  CHECK(sw::formatValue(true) == "1" && sw::formatValue(false) == "0");
  CHECK(sw::formatValue(uint32_t(128)) == "128");
  CHECK(sw::formatValue(50.0) == "50" && sw::formatValue(16.5) == "16.5");
  CHECK(sw::formatValue(std::string("x y")) == "x y");

  // the fallback stands for unset/empty in every grammar
  CHECK(sw::legacy::notZero("", true) == true);
  CHECK(sw::legacy::exactlyOne("", false) == false);
  CHECK(sw::legacy::strtoul32("", 9) == 9);
  CHECK(sw::legacy::ulongOneOf<32, 64, 128>("", 32) == 32);
  CHECK(sw::legacy::ulongOneOf<32, 64, 128>("7", 32) == 32);
  CHECK(sw::legacy::ulongOneOf<32, 64, 128>(" 128x", 32) == 128);
  CHECK(sw::legacy::ulongIsOneOf<32, 64, 128>("064")); // strtoul base 10: leading zeros are decimal
  CHECK(!sw::legacy::ulongIsOneOf<32, 64, 128>("0x40"));
}

// --- the startup line, byte for byte

// The values LogStartup takes from the code that owns them.
struct LineValues {
  double hitch_ms = 50;
  int present_split = 1, display_sync = 1;
  std::string deadline = "refresh";
  int cap_limiter = 1;
  uint32_t bg_fps = 10, bg_fps_minimized = 5;
  const char *bg_policy = "focus";
  int blend_hash = 1, mru = 1, topology_keep = 1;
  uint32_t ring = 32;
  std::string max_latency = "unset";
  double compile_wait_ms = 2;
  const char *archive = "0";
  std::string archive_dir = "default";
  uint32_t flush_every = 64;
  uint32_t promote_min = 0, promote_ms = 4;
  const char *timestamp_mode = "estimate";
  int map_fast = 1, skip_rebind = 1;
  std::string control = "off";
  const char *residency = "1", *unretained = "1", *lifetime_check = "0", *metal4 = "1";
  const char *subsample = "off";
  int pso_record = 0;
  unsigned cpus = 8;
  double refresh = 60;
};

// pre-R1 LogStartup's format, verbatim (gw2-on-mac 0034ffa, d3d11_swapchain.cpp), reading `env` for
// the switches it read itself
static std::string
legacyLine(const LineValues &x, Env &env) {
  return std::format(
      "DXMT_STATS=1 DXMT_HITCH_MS={} DXMT_PRESENT_SPLIT={} DXMT_DISPLAY_SYNC={} DXMT_PRESENT_DEADLINE={} "
      "DXMT_CAP_LIMITER={} DXMT_STENCIL_PFV={} DXMT_DEPTH_PFV={} DXMT_BG_FPS={} DXMT_BG_FPS_MINIMIZED={} "
      "DXMT_BG_POLICY={} DXMT_RENDER_THREAD_PRIORITY={} DXMT_PSO_BLEND_HASH={} DXMT_PSO_MRU={} "
      "DXMT_PSO_TOPOLOGY_KEEP={} DXMT_CHUNKS={} DXMT_MAX_FRAME_LATENCY={} DXMT_COMPILE_WAIT_MS={} "
      "DXMT_PSO_ARCHIVE={} DXMT_PSO_ARCHIVE_DIR={} DXMT_PSO_FLUSH_EVERY={} DXMT_PROMOTE_FLUSH_MIN={} "
      "DXMT_PROMOTE_FLUSH_MS={} DXMT_TIMESTAMP_MODE={} DXMT_MAP_FAST={} DXMT_SKIP_REBIND={} DXMT_CONTROL={} "
      "DXMT_RESIDENCY_SET={} DXMT_UNRETAINED={} DXMT_LIFETIME_CHECK={} DXMT_METAL4={} "
      "DXMT_SUBSAMPLE_METALFX={} DXMT_PSO_RECORD={} cpus={} refresh={}",
      x.hitch_ms, x.present_split, x.display_sync, x.deadline, x.cap_limiter,
      int(env.get("DXMT_STENCIL_PFV") == "1"), int(env.get("DXMT_DEPTH_PFV") == "1"), x.bg_fps,
      x.bg_fps_minimized, x.bg_policy, std::strtoul(env.get("DXMT_RENDER_THREAD_PRIORITY").c_str(), nullptr, 10),
      x.blend_hash, x.mru, x.topology_keep, x.ring, x.max_latency, x.compile_wait_ms, x.archive, x.archive_dir,
      x.flush_every, x.promote_min, x.promote_ms, x.timestamp_mode, x.map_fast, x.skip_rebind, x.control,
      x.residency, x.unretained, x.lifetime_check, x.metal4, x.subsample, x.pso_record, x.cpus, x.refresh
  );
}

// R1's LogStartup field list (same names, order and values as d3d11_swapchain.cpp)
static std::vector<sw::Field>
generatedFields(const LineValues &x) {
  return {
      {"DXMT_STATS", std::nullopt},
      {"DXMT_HITCH_MS", std::format("{}", x.hitch_ms)},
      {"DXMT_PRESENT_SPLIT", std::format("{}", x.present_split)},
      {"DXMT_DISPLAY_SYNC", std::format("{}", x.display_sync)},
      {"DXMT_PRESENT_DEADLINE", x.deadline},
      {"DXMT_CAP_LIMITER", std::format("{}", x.cap_limiter)},
      {"DXMT_STENCIL_PFV", std::nullopt},
      {"DXMT_DEPTH_PFV", std::nullopt},
      {"DXMT_BG_FPS", std::format("{}", x.bg_fps)},
      {"DXMT_BG_FPS_MINIMIZED", std::format("{}", x.bg_fps_minimized)},
      {"DXMT_BG_POLICY", x.bg_policy},
      {"DXMT_RENDER_THREAD_PRIORITY", std::nullopt},
      {"DXMT_PSO_BLEND_HASH", std::format("{}", x.blend_hash)},
      {"DXMT_PSO_MRU", std::format("{}", x.mru)},
      {"DXMT_PSO_TOPOLOGY_KEEP", std::format("{}", x.topology_keep)},
      {"DXMT_CHUNKS", std::format("{}", x.ring)},
      {"DXMT_MAX_FRAME_LATENCY", x.max_latency},
      {"DXMT_COMPILE_WAIT_MS", std::format("{}", x.compile_wait_ms)},
      {"DXMT_PSO_ARCHIVE", x.archive},
      {"DXMT_PSO_ARCHIVE_DIR", x.archive_dir},
      {"DXMT_PSO_FLUSH_EVERY", std::format("{}", x.flush_every)},
      {"DXMT_PROMOTE_FLUSH_MIN", std::format("{}", x.promote_min)},
      {"DXMT_PROMOTE_FLUSH_MS", std::format("{}", x.promote_ms)},
      {"DXMT_TIMESTAMP_MODE", x.timestamp_mode},
      {"DXMT_MAP_FAST", std::format("{}", x.map_fast)},
      {"DXMT_SKIP_REBIND", std::format("{}", x.skip_rebind)},
      {"DXMT_CONTROL", x.control},
      {"DXMT_RESIDENCY_SET", x.residency},
      {"DXMT_UNRETAINED", x.unretained},
      {"DXMT_LIFETIME_CHECK", x.lifetime_check},
      {"DXMT_METAL4", x.metal4},
      {"DXMT_SUBSAMPLE_METALFX", x.subsample},
      {"DXMT_PSO_RECORD", std::format("{}", x.pso_record)},
  };
}

static std::vector<sw::Field>
generatedTail(const LineValues &x) {
  return {{"cpus", std::format("{}", x.cpus)}, {"refresh", std::format("{}", x.refresh)}};
}

static void
checkLine(const LineValues &x, const std::map<std::string, std::string> &vars, const std::string &what) {
  Env env, env2;
  env.vars = env2.vars = vars;
  Legacy old(env);
  Migrated now(env2);
  bool on = old.statsEnabled();
  CHECK(now.statsEnabled() == on);
  if (!on)
    return; // the line is logged only with STATS on
  auto legacy = legacyLine(x, env);
  auto generated = sw::startupLine(now.registry, generatedFields(x), generatedTail(x));
  CHECK_EQ(generated, legacy, what);
  CHECK_EQ(env2.reads, env.reads, what + ": source reads");

  // the comparison sees order: swapping two neighbouring fields, or moving one, must differ
  auto fields = generatedFields(x);
  std::swap(fields[1], fields[2]);
  CHECK(sw::startupLine(now.registry, fields, generatedTail(x)) != legacy);
  fields = generatedFields(x);
  auto moved = fields[6];
  fields.erase(fields.begin() + 6);
  fields.push_back(moved);
  CHECK(sw::startupLine(now.registry, fields, generatedTail(x)) != legacy);
}

static void
testStartupLine() {
  LineValues defaults;
  checkLine(defaults, {}, "defaults");

  LineValues set;
  set.hitch_ms = 33.3;
  set.present_split = 0;
  set.deadline = "4.5";
  set.bg_policy = "occlusion";
  set.ring = 128;
  set.max_latency = "3";
  set.archive = "failed";
  set.archive_dir = "/Users/x/My Cache"; // a value with a space is written as it is
  set.control = "/tmp/dxmt control.txt";
  set.residency = "failed";
  set.lifetime_check = "unbuilt";
  set.metal4 = "unsupported";
  set.subsample = "auto";
  set.pso_record = 1;
  set.cpus = 18;
  set.refresh = 119.88;
  checkLine(set,
            {{"DXMT_STENCIL_PFV", "1"},
             {"DXMT_DEPTH_PFV", "1"},
             {"DXMT_RENDER_THREAD_PRIORITY", "3"},
             {"DXMT_CHUNKS", "128"},
             {"DXMT_STATS", "1"}},
            "all set");
  checkLine(defaults,
            {{"DXMT_STENCIL_PFV", "true"},
             {"DXMT_DEPTH_PFV", " 1"},
             {"DXMT_RENDER_THREAD_PRIORITY", "7x"},
             {"DXMT_CHUNKS", "100"},
             {"DXMT_STATS", "yes"}},
            "malformed");
  checkLine(defaults,
            {{"DXMT_STENCIL_PFV", ""},
             {"DXMT_DEPTH_PFV", "0"},
             {"DXMT_RENDER_THREAD_PRIORITY", ""},
             {"DXMT_CHUNKS", ""},
             {"DXMT_STATS", ""}},
            "empty");
  checkLine(defaults, {{"DXMT_RENDER_THREAD_PRIORITY", "+3"}, {"DXMT_CHUNKS", "064"}}, "signs and zeros");

  // a switch declared later and not named by LogStartup: after DXMT_PSO_RECORD, by name, before cpus
  Env env;
  Migrated now(env);
  sw::Switch<bool> zeta{"DXMT_ZETA", true, sw::legacy::notZero, sw::Line::Listed, now.registry};
  sw::Switch<bool> alpha{"DXMT_ALPHA", false, sw::legacy::exactlyOne, sw::Line::Listed, now.registry};
  auto line = sw::startupLine(now.registry, generatedFields(defaults), generatedTail(defaults));
  CHECK(line.ends_with("DXMT_PSO_RECORD=0 DXMT_ALPHA=0 DXMT_ZETA=1 cpus=8 refresh=60"));
  CHECK(line.starts_with("DXMT_STATS=1 DXMT_HITCH_MS=50 "));
}

// gw2 (#227): DXMT_MTL4_PRESENT, declared as dxmt_gw2_metal4.cpp declares it, with its grammar from
// dxmt_gw2_present.hpp: exactly `mtl3`, anything else `mtl4`; on the startup line by name
static void
testMtl4Present() {
  Env env;
  Migrated now(env);
  sw::Switch<std::string> present{"DXMT_MTL4_PRESENT", "mtl4", gw2::mtl4PresentGrammar, sw::Line::Listed,
                                  now.registry};
  CHECK_EQ(present(), std::string("mtl4"), "unset");
  for (const char *other : {"", "mtl4", "MTL3", "mtl3 ", " mtl3", "mtl", "3", "1", "metal3", "mtl33"}) {
    env.vars["DXMT_MTL4_PRESENT"] = other;
    CHECK_EQ(present(), std::string("mtl4"), std::string("\"") + other + "\"");
  }
  env.vars["DXMT_MTL4_PRESENT"] = "mtl3";
  CHECK_EQ(present(), std::string("mtl3"), "mtl3");
  CHECK_EQ(*now.registry.effective("DXMT_MTL4_PRESENT"), std::string("mtl3"), "line value");
  LineValues defaults;
  auto line = sw::startupLine(now.registry, generatedFields(defaults), generatedTail(defaults));
  CHECK(line.find(" DXMT_MTL4_PRESENT=mtl3 ") != std::string::npos);
  env.vars.erase("DXMT_MTL4_PRESENT");
  CHECK_EQ(*now.registry.effective("DXMT_MTL4_PRESENT"), std::string("mtl4"), "line value, unset");
}

// gw2 (#226): DXMT_MTL4_LAYER_SET, declared as dxmt_presenter.cpp declares it: `queue`, or `buffer`
// for anything else, and on the startup line by name
static void
testLayerSet() {
  const std::vector<std::pair<std::optional<std::string>, std::string>> cases = {
      {std::nullopt, "buffer"}, {"", "buffer"},     {"buffer", "buffer"}, {"queue", "queue"},
      {"QUEUE", "buffer"},      {" queue", "buffer"}, {"queue ", "buffer"}, {"1", "buffer"},
      {"0", "buffer"},          {"queues", "buffer"},
  };
  for (auto &[input, want] : cases) {
    Env env;
    if (input)
      env.vars[gw2::kLayerSetSwitch] = *input;
    sw::Registry registry(env.source());
    sw::Switch<std::string> layer_set{gw2::kLayerSetSwitch, gw2::kLayerSetDefault, gw2::layerSetValue,
                                      sw::Line::Listed, registry};
    CHECK_EQ(layer_set(), want, "DXMT_MTL4_LAYER_SET=" + show(input));
    CHECK_EQ(sw::startupLine(registry, {}), "DXMT_MTL4_LAYER_SET=" + want, "startup line, " + show(input));
  }
}

int
main() {
  testMtl4Present();
  testLayerSet();
  testMigratedGrammars();
  testReadSequences();
  testRequestedVsEffective();
  testRegistryRules();
  testStartupLine();
  if (failures) {
    std::printf("switches_test: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("switches_test: all %d checks passed\n", checks);
  return 0;
}
