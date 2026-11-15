// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Itests/gw2/shim -Isrc/dxmt tests/gw2/d24_bias_count_test.cpp src/dxmt/dxmt_gw2_cmds.cpp -o $OUT
// gw2-run: "$OUT" on
// gw2-run: "$OUT" off
//
// gw2 (#266): ENCODE's d24bias and d24slope, and the RASTER line's text (src/dxmt/dxmt_gw2_cmds.hpp). The
// immediate context (src/d3d11/d3d11_context_impl.cpp, PreDraw) works out a render pass's bias with
// d24PassBias when it applies the rasterizer state (at each new pass and each RSSetState), only with
// DXMT_STATS on, and calls FrontCounters::d24Draw for every draw. The context isn't built here, so the
// test plays those two steps for a known mix of passes and draws: biased and not, slope only, an emulated
// D24 target and not, triangles, lines, points and patches, a state change inside a pass. It publishes
// the counters over two chunks, as commits do, and reads both fields back from the ENCODE section.
// DXMT_STATS comes from the argument (this file's statsEnabled), so it runs twice:
//
// - on: d24bias and d24slope per frame, after the other fields; the next window reads 0.0.
// - off: nothing is counted, and both fields read 0.0.
//
// Run: tests/gw2/run.zsh pure, or the commands above with $OUT set.

#include "dxmt_gw2_cmds.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// --- what the rest of DXMT provides
static bool g_stats = false;
namespace dxmt::gw2 {
static SectionFn g_encode = nullptr;
void
registerSection(const char *name, SectionFn fn) {
  if (std::string(name) == "ENCODE")
    g_encode = fn;
}
bool
statsEnabled() {
  return g_stats;
}
} // namespace dxmt::gw2

using namespace dxmt::gw2;

// D3D11_PRIMITIVE_TOPOLOGY
constexpr uint32_t Points = 1, LineList = 2, LineStrip = 3, TriList = 4, TriStrip = 5, LineListAdj = 10,
                   LineStripAdj = 11, TriListAdj = 12, TriStripAdj = 13, Patch3 = 35, Patch32 = 64;

struct State {
  int32_t bias;
  float slope;
};

// A rasterizer state applied inside a pass, then its draws
struct Run {
  State state;
  std::vector<uint32_t> topologies;
};

struct Pass {
  bool d24; // the depth target is an emulated D24 texture (false: D32S8, another format, or none)
  std::vector<Run> runs;
};

// PreDraw's two steps
static void
play(FrontCounters &front, DepthBias &pass_bias, const Pass &pass) {
  for (auto &run : pass.runs) {
    if (front.on)
      pass_bias = d24PassBias(pass.d24, depthBiasOf(run.state.bias, run.state.slope));
    for (uint32_t topology : run.topologies)
      front.d24Draw(pass_bias, topology);
  }
}

static std::string
field(const std::string &line, const char *name) {
  auto at = line.find(std::string(" ") + name + "=");
  if (at == std::string::npos)
    return "(missing)";
  at += std::strlen(name) + 2;
  return line.substr(at, line.find(' ', at) - at);
}

int
main(int argc, char **argv) {
  bool on = argc > 1 && std::strcmp(argv[1], "on") == 0;
  if (argc < 2 || (!on && std::strcmp(argv[1], "off") != 0)) {
    std::printf("usage: %s on|off\n", argv[0]);
    return 2;
  }
  g_stats = on;

  CHECK(depthBiasOf(0, 0.0f) == DepthBias::None);
  CHECK(depthBiasOf(0, -0.0f) == DepthBias::None);
  CHECK(depthBiasOf(1, 0.0f) == DepthBias::Constant);
  CHECK(depthBiasOf(-50, 2.0f) == DepthBias::Constant);
  CHECK(depthBiasOf(0, 1.5f) == DepthBias::SlopeOnly);
  CHECK(depthBiasOf(0, -1.0f) == DepthBias::SlopeOnly);

  CHECK(rasterLine(0, 0.0f, 0.0f, 3, 3).empty());
  CHECK(rasterLine(0, 0.0f, 0.5f, 3, 3).empty()); // a clamp alone biases nothing
  CHECK(rasterLine(100, 1.5f, 0.0f, 3, 3) == "RASTER bias=100 slope=1.5 clamp=0 fill=solid cull=back");
  CHECK(rasterLine(0, 2.0f, 0.01f, 2, 1) == "RASTER bias=0 slope=2 clamp=0.01 fill=wireframe cull=none");
  CHECK(rasterLine(-8, 0.0f, 0.0f, 3, 2) == "RASTER bias=-8 slope=0 clamp=0 fill=solid cull=front");

  for (uint32_t t : {TriList, TriStrip, TriListAdj, TriStripAdj, Patch3, Patch32})
    CHECK(trianglesTopology(t));
  for (uint32_t t : {0u, Points, LineList, LineStrip, LineListAdj, LineStripAdj, 6u, 32u, 65u})
    CHECK(!trianglesTopology(t));

  CHECK(g_encode != nullptr);
  if (!g_encode)
    return 1;

  const State none{0, 0.0f}, bias{100, 0.0f}, slope{0, 1.5f}, both{-50, 2.0f};
  const std::vector<Pass> chunk1 = {
      // an emulated D24 target without a bias: nothing
      {true, {{none, {TriList, TriList, TriStrip, TriList, TriList}}}},
      // biased: the triangles (list, strip, adjacency, patches) count, lines and points don't; then a
      // slope-only state inside the same pass
      {true,
       {{bias, {TriList, TriList, TriList, TriStrip, TriStrip, LineList, LineList, LineStrip, LineList, Points,
                TriListAdj, Patch3}},
        {slope, {TriList, TriList, LineStrip}}}},
      // biased into a target that isn't an emulated D24 one: nothing
      {false, {{bias, {TriList, TriList, TriList, TriList, TriList, TriList}}}},
  };
  const std::vector<Pass> chunk2 = {
      // no depth target
      {false, {{slope, {TriList, TriList, TriList}}}},
      // slope only
      {true, {{slope, {TriList, TriList, TriList, TriList, LineListAdj, LineListAdj}}}},
      // a constant bias with a slope counts as d24bias
      {true, {{both, {TriStripAdj}}}},
  };
  constexpr unsigned kBias = 7 + 1, kSlope = 2 + 4; // the draws counted above

  FrontCounters front;
  CHECK(front.on == on);
  DepthBias pass_bias = DepthBias::None;
  for (auto &pass : chunk1)
    play(front, pass_bias, pass);
  CHECK(front.d24_bias == (on ? 7u : 0u));
  CHECK(front.d24_slope == (on ? 2u : 0u));
  front.publish();
  for (auto &pass : chunk2)
    play(front, pass_bias, pass);
  front.publish();
  CHECK(front.d24_bias == 0 && front.d24_slope == 0);

  // two frames in the window: per frame
  std::string line = g_encode(2000.0, 2);
  std::printf("RESULT ENCODE %s\n", line.c_str());
  CHECK(line.rfind("draws=", 0) == 0);
  CHECK(line.find(" refuse=") < line.find(" d24bias="));
  CHECK(line.find(" d24bias=") < line.find(" d24slope="));
  char want_bias[16], want_slope[16];
  std::snprintf(want_bias, sizeof want_bias, "%.1f", on ? kBias / 2.0 : 0.0);
  std::snprintf(want_slope, sizeof want_slope, "%.1f", on ? kSlope / 2.0 : 0.0);
  CHECK(field(line, "d24bias") == want_bias);
  CHECK(field(line, "d24slope") == want_slope);

  // drained: the next window starts at 0
  line = g_encode(2000.0, 1);
  CHECK(field(line, "d24bias") == "0.0");
  CHECK(field(line, "d24slope") == "0.0");

  std::printf("%s: %s\n", argv[1], failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
