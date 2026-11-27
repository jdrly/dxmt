// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Itests/gw2/shim -Isrc/dxmt tests/gw2/setter_count_test.cpp src/dxmt/dxmt_gw2_setters.cpp -o $OUT
// gw2-run: "$OUT" on
// gw2-run: "$OUT" off
//
// gw2 (#365): the PSO line's setter fields (src/dxmt/dxmt_gw2_setters.hpp). The context isn't built here (it
// needs the PE toolchain and Metal), so its steps are copied from src/d3d11/d3d11_context_impl.cpp: the state
// OMSetBlendState, OMSetDepthStencilState and RSSetState store and mark dirty, with the counter hook where the
// context has it, SOSetTargets's count, a new render pass marking all three states, and PreDraw emitting one
// command per marked state. The test sets each state twice, publishes the counts over two chunks as commits
// do, and reads the seven fields back from setterFields:
//
// - on: samefactor, samedss, nullrs and soset count only the calls that set what the context has (a changed
//   factor, a factor of -0 for 0, a changed state or reference, RSSetState(NULL) with a state bound don't), and
//   blendcmd, dsscmd and rscmd equal the commands the draws emitted; the next window reads 0.0.
// - off: nothing is counted, and every field reads 0.0.
//
// Run: tests/gw2/run.zsh pure, or the commands above with $OUT set.

#include "dxmt_gw2_setters.hpp"
#include <cstdio>
#include <cstring>
#include <string>

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
bool
statsEnabled() {
  return g_stats;
}
} // namespace dxmt::gw2

using namespace dxmt::gw2;

// Stand-ins for the state objects: only their addresses matter.
struct Dss {};
struct Rs {};

// The context's part of the four setters and of PreDraw.
struct Context {
  SetterCounters setters;
  float blend_factor[4] = {}; // state_ = {}: zeros, not D3D's 1, 1, 1, 1
  const Dss *dss = nullptr;
  uint32_t stencil_ref = 0;
  const Rs *rs = nullptr;
  bool dirty_blend = false, dirty_dss = false, dirty_rs = false;
  // the commands PreDraw emitted
  unsigned emitted_blend = 0, emitted_dss = 0, emitted_rs = 0;

  void
  omSetBlendState(const float *factor) {
    setters.blendFactor(blend_factor, factor);
    if (factor)
      std::memcpy(blend_factor, factor, sizeof(float[4]));
    else
      blend_factor[0] = blend_factor[1] = blend_factor[2] = blend_factor[3] = 1.0f;
    dirty_blend = true;
  }
  void
  omSetDepthStencilState(const Dss *state, uint32_t ref) {
    SameDepthStencil same(setters, dss, stencil_ref);
    dss = state;
    stencil_ref = ref;
    dirty_dss = true;
  }
  void
  rsSetState(const Rs *state) {
    if (state) {
      if (rs == state)
        return;
      rs = state;
      dirty_rs = true;
    } else {
      setters.nullRasterizer(rs);
      rs = nullptr;
      dirty_rs = true;
    }
  }
  void
  soSetTargets() {
    setters.count(Setter::SoSet);
  }
  // SwitchToRenderEncoder
  void
  newPass() {
    dirty_blend = dirty_dss = dirty_rs = true;
  }
  // PreDraw, then clrAll
  void
  draw() {
    if (dirty_dss) {
      setters.count(Setter::DssCmd);
      emitted_dss++;
    }
    if (dirty_rs) {
      setters.count(Setter::RsCmd);
      emitted_rs++;
    }
    if (dirty_blend) {
      setters.count(Setter::BlendCmd);
      emitted_blend++;
    }
    dirty_blend = dirty_dss = dirty_rs = false;
  }
};

static double
field(const std::string &line, const char *name) {
  std::string key = std::string(name) + "=";
  size_t at = line.rfind(" " + key);
  at = at == std::string::npos ? (line.starts_with(key) ? 0 : std::string::npos) : at + 1;
  if (at == std::string::npos) {
    std::printf("FAIL no %s in \"%s\"\n", name, line.c_str());
    failures++;
    return -1;
  }
  return std::stod(line.substr(at + key.size()));
}

int
main(int argc, char **argv) {
  bool on = argc > 1 && std::strcmp(argv[1], "on") == 0;
  if (argc < 2 || (!on && std::strcmp(argv[1], "off") != 0)) {
    std::printf("usage: %s on|off\n", argv[0]);
    return 2;
  }
  g_stats = on;

  const float ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float half[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const float negzero[4] = {-0.0f, 0.0f, 0.0f, 0.0f};
  CHECK(sameBlendFactor(ones, nullptr));
  CHECK(sameBlendFactor(ones, ones));
  CHECK(!sameBlendFactor(half, nullptr));
  CHECK(!sameBlendFactor(zero, negzero)); // equal as floats, not as the command's bits

  Dss dss_a, dss_b;
  Rs rs_a;
  unsigned want_factor = 0, want_dss = 0, want_rs = 0, want_so = 0;

  // chunk 1
  Context ctx;
  ctx.newPass();
  ctx.draw(); // all three commands
  ctx.omSetBlendState(nullptr); // 1, 1, 1, 1 for the context's zeros: changed
  ctx.omSetBlendState(nullptr);
  want_factor++;
  ctx.omSetBlendState(half);
  ctx.omSetBlendState(half); // same
  want_factor++;
  ctx.omSetBlendState(nullptr); // 1, 1, 1, 1 for 0.5: changed
  ctx.omSetBlendState(ones);    // the same as null's
  want_factor++;
  ctx.draw(); // blend
  ctx.omSetDepthStencilState(&dss_a, 1);
  ctx.omSetDepthStencilState(&dss_a, 1); // same
  want_dss++;
  ctx.omSetDepthStencilState(&dss_a, 2); // changed reference
  ctx.omSetDepthStencilState(&dss_b, 2); // changed state
  ctx.omSetDepthStencilState(nullptr, 2);
  ctx.omSetDepthStencilState(nullptr, 2); // null on null, same reference
  want_dss++;
  ctx.draw(); // dss
  ctx.rsSetState(nullptr); // none set yet
  want_rs++;
  ctx.rsSetState(nullptr);
  want_rs++;
  ctx.draw(); // rs
  ctx.setters.publish();

  // chunk 2: a deferred context's, or the next commit's
  ctx.rsSetState(&rs_a);
  ctx.rsSetState(&rs_a); // returns before it marks anything, counted nowhere
  ctx.rsSetState(nullptr); // a state was bound
  ctx.rsSetState(nullptr);
  want_rs++;
  ctx.omSetBlendState(zero);
  ctx.omSetBlendState(negzero); // a different bit pattern
  ctx.omSetBlendState(negzero);
  want_factor++;
  ctx.soSetTargets();
  ctx.soSetTargets();
  want_so += 2;
  ctx.newPass();
  ctx.draw(); // all three
  ctx.draw(); // nothing marked: none
  ctx.omSetDepthStencilState(nullptr, 2);
  want_dss++;
  ctx.draw(); // dss
  ctx.setters.publish();

  CHECK(ctx.emitted_blend == 3 && ctx.emitted_dss == 4 && ctx.emitted_rs == 3);

  const uint32_t frames = 2;
  bool any = !on;
  std::string line = setterFields(frames, any);
  std::printf("%s: %s\n", on ? "on" : "off", line.c_str());
  CHECK(line.starts_with("samefactor="));
  CHECK(any == on);
  auto per_frame = [&](unsigned count) { return on ? double(count) / frames : 0.0; };
  // to one decimal, as the line prints them
  auto approx = [](double a, double b) { return a > b - 0.051 && a < b + 0.051; };
  CHECK(approx(field(line, "samefactor"), per_frame(want_factor)));
  CHECK(approx(field(line, "samedss"), per_frame(want_dss)));
  CHECK(approx(field(line, "nullrs"), per_frame(want_rs)));
  CHECK(approx(field(line, "soset"), per_frame(want_so)));
  CHECK(approx(field(line, "blendcmd"), per_frame(ctx.emitted_blend)));
  CHECK(approx(field(line, "dsscmd"), per_frame(ctx.emitted_dss)));
  CHECK(approx(field(line, "rscmd"), per_frame(ctx.emitted_rs)));

  // the next window: drained
  line = setterFields(frames, any);
  CHECK(!any);
  CHECK(line == "samefactor=0.0 samedss=0.0 nullrs=0.0 soset=0.0 blendcmd=0.0 dsscmd=0.0 rscmd=0.0");

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
