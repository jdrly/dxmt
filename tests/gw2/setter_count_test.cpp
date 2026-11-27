// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Itests/gw2/shim -Isrc/dxmt tests/gw2/setter_count_test.cpp src/dxmt/dxmt_gw2_setters.cpp -o $OUT
// gw2-run: "$OUT" on skip
// gw2-run: "$OUT" on noskip
// gw2-run: "$OUT" off skip
// gw2-run: "$OUT" off noskip
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
// gw2 (#369): the second argument is DXMT_SKIP_SAME_STATE. The copied setters call the fork's skipSameBlend and
// SameDepthStencil::skipSame where the context does. With skip, an OMSetBlendState with the state, factor and
// mask the context has, and an OMSetDepthStencilState with its state and reference, mark nothing, so a draw after
// them emits nothing; a changed factor, mask, blend state, depth-stencil state or reference still marks. With
// noskip every call marks as before. samefactor and samedss read the same either way, with DXMT_STATS on or off.
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
struct Blend {};
struct Dss {};
struct Rs {};

// The fields of D3D11OutputMergerStageState the two setters use, as state_ = {} leaves them.
struct OutputMerger {
  const Dss *DepthStencilState = nullptr;
  uint32_t StencilRef = 0;
  const Blend *BlendState = nullptr;
  float BlendFactor[4] = {}; // zeros, not D3D's 1, 1, 1, 1
  uint32_t SampleMask = 0xffffffff;
};

// The context's part of the four setters and of PreDraw.
struct Context {
  bool skip; // DXMT_SKIP_SAME_STATE
  SetterCounters setters;
  OutputMerger om;
  const Rs *rs = nullptr;
  bool dirty_blend = false, dirty_dss = false, dirty_rs = false;
  // the commands PreDraw emitted, and the pipeline invalidations
  unsigned emitted_blend = 0, emitted_dss = 0, emitted_rs = 0, invalidated = 0;

  explicit Context(bool skip_same_state) : skip(skip_same_state) {}

  void
  omSetBlendState(const float *factor, const Blend *state = nullptr, uint32_t mask = 0xffffffff) {
    if (setters.skipSameBlend(skip, om, state, factor, mask))
      return;
    bool invalidate = false;
    if (state != om.BlendState) {
      om.BlendState = state;
      invalidate = true;
    }
    setters.blendFactor(om.BlendFactor, factor);
    if (factor)
      std::memcpy(om.BlendFactor, factor, sizeof(float[4]));
    else
      om.BlendFactor[0] = om.BlendFactor[1] = om.BlendFactor[2] = om.BlendFactor[3] = 1.0f;
    if (om.SampleMask != mask) {
      om.SampleMask = mask;
      invalidate = true;
    }
    if (invalidate)
      invalidated++;
    dirty_blend = true;
  }
  void
  omSetDepthStencilState(const Dss *state, uint32_t ref) {
    SameDepthStencil same(setters, om.DepthStencilState, om.StencilRef);
    if (same.skipSame(skip, state, ref))
      return;
    om.DepthStencilState = state;
    om.StencilRef = ref;
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
  bool on = argc > 2 && std::strcmp(argv[1], "on") == 0;
  bool skip = argc > 2 && std::strcmp(argv[2], "skip") == 0;
  if (argc < 3 || (!on && std::strcmp(argv[1], "off") != 0) || (!skip && std::strcmp(argv[2], "noskip") != 0)) {
    std::printf("usage: %s on|off skip|noskip\n", argv[0]);
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
  Context ctx(skip);
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
  ctx.omSetDepthStencilState(nullptr, 2); // the same, after a draw
  want_dss++;
  ctx.draw(); // dss, without the switch
  ctx.omSetBlendState(negzero); // the same, after a draw
  want_factor++;
  ctx.draw(); // blend, without the switch
  ctx.setters.publish();

  // the switch saves the two commands after a draw; the same calls before a draw saved nothing
  CHECK(ctx.emitted_blend == (skip ? 3u : 4u) && ctx.emitted_dss == (skip ? 3u : 4u) && ctx.emitted_rs == 3);

  // Each call's mark, on a context that never publishes: a draw clears the marks, then one call.
  {
    Context m(skip);
    Blend blend_a, blend_b;
    unsigned invalidated = 0;
    auto blend = [&](const float *factor, const Blend *state, uint32_t mask) {
      m.draw();
      m.omSetBlendState(factor, state, mask);
      return m.dirty_blend;
    };
    auto dss = [&](const Dss *state, uint32_t ref) {
      m.draw();
      m.omSetDepthStencilState(state, ref);
      return m.dirty_dss;
    };
    CHECK(blend(zero, nullptr, 0xffffffff) == !skip); // what state_ = {} holds
    CHECK(dss(nullptr, 0) == !skip);
    m.newPass();
    m.omSetBlendState(half, &blend_a, 0xff);
    m.omSetDepthStencilState(&dss_a, 1);
    m.draw();
    invalidated = m.invalidated;
    CHECK(blend(half, &blend_a, 0xff) == !skip); // the same
    CHECK(m.invalidated == invalidated);
    CHECK(blend(ones, &blend_a, 0xff));          // factor
    CHECK(m.invalidated == invalidated);
    CHECK(blend(nullptr, &blend_a, 0xff) == !skip); // null is 1, 1, 1, 1: the same
    CHECK(blend(ones, &blend_a, 0x0f));          // mask
    CHECK(m.invalidated == ++invalidated);
    CHECK(blend(ones, &blend_b, 0x0f));          // blend state
    CHECK(m.invalidated == ++invalidated);
    CHECK(blend(ones, nullptr, 0x0f));           // blend state, to none
    CHECK(m.invalidated == ++invalidated);
    CHECK(blend(ones, nullptr, 0x0f) == !skip);  // none on none
    CHECK(m.om.BlendState == nullptr && m.om.SampleMask == 0x0f && sameBlendFactor(m.om.BlendFactor, ones));
    CHECK(dss(&dss_a, 1) == !skip); // the same
    CHECK(dss(&dss_a, 2));          // reference
    CHECK(dss(&dss_b, 2));          // state
    CHECK(dss(nullptr, 2));         // state, to none
    CHECK(dss(nullptr, 2) == !skip); // none on none
    CHECK(dss(nullptr, 0));         // reference
    CHECK(m.om.DepthStencilState == nullptr && m.om.StencilRef == 0);
  }

  const uint32_t frames = 2;
  bool any = !on;
  std::string line = setterFields(frames, any);
  std::printf("%s %s: %s\n", on ? "on" : "off", skip ? "skip" : "noskip", line.c_str());
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
