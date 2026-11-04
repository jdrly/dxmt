// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O2 -Wall -Wextra -Isrc/dxmt -Isrc/util tests/gw2/dependency_compression_test.cpp src/dxmt/dxmt_deptrack.cpp -o $OUT
//
// gw2 (#132): the encoder dependency policies (src/dxmt/dxmt_deptrack.*) against an independent hazard
// oracle. No Metal, no Wine.
//
// - A stream of encoders (compute/blit with one ID; render with a pre-raster ID and a fragment ID) accesses
//   resources the way ArgumentEncodingContext::track and endPass do: the production GenericAccessTracker
//   calls with the context's horizon, then FenceLocalityCheck::collectAndSimplifyWaits per stage.
// - The oracle models Metal's fences: every stage updates the fence slot `id % kParityLane` when it ends; a
//   wait on a slot waits for every update of that slot encoded before the waiting encoder (MTL4CommandEncoder.h,
//   "Synchronizing passes with a fence"); a render encoder's fragment stage follows its own pre-raster stage.
//   Happens-before is the transitive closure.
// - Every RAW/WAR/WAW pair between different encoders must be ordered by happens-before. Checking each
//   access against the resource's last writer and the readers since then covers every pair by transitivity.
// - Both policies must have zero violations. A control run (the strong waits only, no window and no
//   checkpoint, which is what dropping the weak window without a replacement means) must show violations,
//   so the oracle is known to bite.
// - Streams: random mixes over hundreds of seeds (with long-lived resources written once and read 300+
//   encoders later, render-target chains, UAV compute, depth), plus scripted cases (100 readers then a
//   writer; a producer 600 encoders back; same-slot producers 256 apart).

#include "dxmt_deptrack.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <tuple>
#include <vector>

using namespace dxmt;

namespace {

enum class Policy { Legacy, Compressed, StrongOnly };

enum class Kind { Compute, Blit, Render };
enum class Stage { Single, PreRaster, Fragment };

struct Access {
  unsigned resource;
  bool write;
  bool uav;
  Stage stage;
};

struct Encoder {
  Kind kind;
  std::vector<Access> accesses;
};

struct Bits {
  std::vector<uint64_t> w;
  explicit Bits(size_t n = 0) : w((n + 63) / 64) {}
  void set(size_t i) { w[i / 64] |= 1ull << (i % 64); }
  bool test(size_t i) const { return w[i / 64] >> (i % 64) & 1; }
  void merge(const Bits &o) {
    for (size_t i = 0; i < w.size(); i++)
      w[i] |= o.w[i];
  }
};

struct Result {
  uint64_t violations = 0;
  std::set<std::tuple<uint64_t, unsigned, uint64_t>> missed; // (encoder, access, prior stage)
  uint64_t waits = 0;
  uint64_t stages = 0;
  uint64_t checked = 0;
};

// The oracle's record of one resource's accesses: node = encoder stage ID, plus the encoder it belongs to
struct Touch {
  EncoderId node;
  uint64_t encoder;
};

bool verbose = false;

Result
run(const std::vector<Encoder> &stream, unsigned resources, Policy policy) {
  FenceLocalityCheck locality;
  locality.setCompressed(policy == Policy::Compressed);
  EncoderId horizon = policy == Policy::Compressed ? kCompressedHorizon : kLane;
  std::vector<GenericAccessTracker> trackers(resources);

  EncoderId next_id = kParityLane; // as the context: never from 0
  EncoderId max_id = next_id + stream.size() * 2 + 2;
  std::vector<Bits> before(max_id + 1);          // happens-before ancestors of each stage node
  // per fence slot: every stage that updated it so far, with their ancestors (a wait covers them all)
  std::vector<Bits> slot_history(kParityLane, Bits(max_id + 1));

  std::vector<Touch> last_write(resources, {0, 0});
  std::vector<std::vector<Touch>> readers(resources);
  Result r;

  auto ordered = [&](EncoderId a, EncoderId b) { return a && before[b].test(a); };

  for (uint64_t e = 0; e < stream.size(); e++) {
    auto &enc = stream[e];
    EncoderBarrierState barriers;
    FenceSet wait, wait_vertex;
    EncoderId id, vid = 0;
    if (enc.kind == Kind::Render) {
      vid = next_id++;
      id = next_id++;
    } else {
      id = next_id++;
    }
    // the production calls, as ArgumentEncodingContext::track does them
    for (auto &a : enc.accesses) {
      auto &t = trackers[a.resource];
      switch (a.stage) {
      case Stage::Single:
        if (a.write)
          t.accessExclusive(id, wait, barriers, a.uav, horizon);
        else
          t.accessShared(id, wait, barriers, horizon);
        break;
      case Stage::PreRaster:
        if (a.write)
          t.accessExclusivePreRaster(vid, wait_vertex, barriers, a.uav, horizon);
        else
          t.accessSharedPreRaster(vid, wait_vertex, barriers, horizon);
        break;
      case Stage::Fragment:
        if (a.write)
          t.accessExclusiveFragment(id, wait, barriers, a.uav, horizon);
        else
          t.accessSharedFragment(id, wait, barriers, horizon);
        break;
      }
    }
    // endPass
    FenceSet emit, emit_vertex;
    if (policy == Policy::StrongOnly) {
      emit_vertex = wait_vertex;
      emit = wait;
      if (vid)
        emit.unset(vid);
    } else if (enc.kind == Kind::Render) {
      emit_vertex = locality.collectAndSimplifyWaits(wait_vertex, vid);
      emit = locality.collectAndSimplifyWaits(wait, id, true);
    } else {
      emit = locality.collectAndSimplifyWaits(wait, id);
    }
    r.waits += emit.count() + emit_vertex.count();
    r.stages += vid ? 2 : 1;

    // Metal's fences: a wait covers every update of its slot encoded before this encoder
    auto edges = [&](FenceSet &set, EncoderId node) {
      before[node] = Bits(max_id + 1);
      set.forEach([&](EncoderId slot) { before[node].merge(slot_history[slot % kParityLane]); });
    };
    if (vid) {
      // a stage named in the vertex set waits at pre-raster, which also orders the fragment stage
      edges(emit_vertex, vid);
      edges(emit, id);
      before[id].set(vid);
      before[id].merge(before[vid]);
    } else {
      edges(emit, id);
    }
    auto update = [&](EncoderId node) {
      auto &h = slot_history[node % kParityLane];
      h.set(node);
      h.merge(before[node]);
    };
    if (vid)
      update(vid);
    update(id);

    // the oracle: every hazard with another encoder must be ordered
    for (auto &a : enc.accesses) {
      EncoderId node = a.stage == Stage::PreRaster ? vid : id;
      auto check = [&](const Touch &prior) {
        if (!prior.node || prior.encoder == e)
          return;
        r.checked++;
        if (!ordered(prior.node, node)) {
          r.violations++;
          r.missed.insert({e, unsigned(&a - enc.accesses.data()), prior.node});
          if (verbose && r.violations <= 3)
            std::printf(
                "    violation: encoder %llu stage %llu %s resource %u after stage %llu (encoder %llu), %llu IDs back\n",
                (unsigned long long)e, (unsigned long long)node, a.write ? "writes" : "reads", a.resource,
                (unsigned long long)prior.node, (unsigned long long)prior.encoder,
                (unsigned long long)(node - prior.node)
            );
        }
      };
      check(last_write[a.resource]);
      if (a.write)
        for (auto &rd : readers[a.resource])
          check(rd);
    }
    for (auto &a : enc.accesses) {
      EncoderId node = a.stage == Stage::PreRaster ? vid : id;
      if (a.write) {
        // a write in the same encoder keeps the later stage as the one later accesses must follow
        if (last_write[a.resource].encoder != e || node > last_write[a.resource].node)
          last_write[a.resource] = {node, e};
        readers[a.resource].clear();
      } else {
        readers[a.resource].push_back({node, e});
      }
    }
  }
  return r;
}

// A random GW2-like stream: render passes over a few targets (chains), sampled textures (some written once
// at the start, read for hundreds of encoders), uploads by blit, UAV compute, depth.
std::vector<Encoder>
randomStream(unsigned seed, unsigned count, unsigned resources, bool pre_raster_writes) {
  std::mt19937 rng(seed);
  auto pick = [&](unsigned n) { return unsigned(rng() % n); };
  std::vector<Encoder> s;
  // the first encoders write every resource once (static textures, buffers)
  for (unsigned r = 0; r < resources; r++)
    s.push_back({Kind::Blit, {{r, true, false, Stage::Single}}});
  unsigned hot = std::max(2u, resources / 4); // the rest are read mostly, written rarely
  for (unsigned i = 0; i < count; i++) {
    Encoder e;
    unsigned k = pick(10);
    e.kind = k < 6 ? Kind::Render : k < 8 ? Kind::Compute : Kind::Blit;
    unsigned n = 1 + pick(5);
    for (unsigned j = 0; j < n; j++) {
      bool rare = pick(4) == 0;
      unsigned r = rare ? hot + pick(resources - hot) : pick(hot);
      bool write = rare ? pick(40) == 0 : pick(3) == 0;
      bool uav = write && pick(4) == 0;
      Stage st = Stage::Single;
      if (e.kind == Kind::Render)
        st = pick(3) == 0 && (pre_raster_writes || !write) ? Stage::PreRaster : Stage::Fragment;
      e.accesses.push_back({r, write, uav, st});
    }
    // a pass's pre-raster accesses before its fragment ones (see main)
    std::stable_partition(e.accesses.begin(), e.accesses.end(), [](const Access &a) {
        return a.stage == Stage::PreRaster;
      });
    s.push_back(std::move(e));
  }
  return s;
}

int failures = 0;

void
expect(bool ok, const char *what) {
  if (!ok) {
    failures++;
    std::printf("FAIL: %s\n", what);
  }
}

void
both(const char *name, const std::vector<Encoder> &s, unsigned resources, bool control_must_fail) {
  auto legacy = run(s, resources, Policy::Legacy);
  auto compressed = run(s, resources, Policy::Compressed);
  auto control = run(s, resources, Policy::StrongOnly);
  std::printf(
      "%-34s stages=%-6llu hazards=%-7llu waits legacy=%-6llu compressed=%-6llu (%.2f vs %.2f a stage) control violations=%llu\n",
      name, (unsigned long long)legacy.stages, (unsigned long long)legacy.checked,
      (unsigned long long)legacy.waits, (unsigned long long)compressed.waits,
      double(legacy.waits) / legacy.stages, double(compressed.waits) / compressed.stages,
      (unsigned long long)control.violations
  );
  expect(legacy.violations == 0, "legacy policy misses a dependency");
  expect(compressed.violations == 0, "compressed policy misses a dependency");
  if (control_must_fail)
    expect(control.violations > 0, "the control (no window, no checkpoint) found no violation: the oracle doesn't bite");
}

} // namespace

int
main(int argc, char **) {
  verbose = argc > 1;
  // scripted: a producer far back, then many readers
  {
    std::vector<Encoder> s;
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});
    for (unsigned i = 0; i < 600; i++)
      s.push_back({Kind::Compute, {{1, true, false, Stage::Single}}});
    s.push_back({Kind::Render, {{0, false, false, Stage::Fragment}}});
    s.push_back({Kind::Render, {{0, false, false, Stage::PreRaster}}});
    s.push_back({Kind::Compute, {{0, true, true, Stage::Single}}});
    both("producer 600 encoders back", s, 2, true);
  }
  // scripted: 100 readers, then a writer; then 100 more readers 300 back
  {
    std::vector<Encoder> s;
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});
    for (unsigned i = 0; i < 100; i++)
      s.push_back({Kind::Render, {{0, false, false, i % 2 ? Stage::Fragment : Stage::PreRaster}}});
    s.push_back({Kind::Compute, {{0, true, false, Stage::Single}}});
    for (unsigned i = 0; i < 300; i++)
      s.push_back({Kind::Blit, {{1, true, false, Stage::Single}}});
    for (unsigned i = 0; i < 100; i++)
      s.push_back({Kind::Render, {{0, false, false, Stage::Fragment}}});
    s.push_back({Kind::Render, {{0, true, false, Stage::Fragment}}});
    both("100 readers then a writer", s, 2, false);
  }
  // scripted: same fence slot, producers 256 IDs apart, read together
  {
    std::vector<Encoder> s;
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});
    for (unsigned i = 0; i < 255; i++)
      s.push_back({Kind::Compute, {{2, false, false, Stage::Single}}});
    s.push_back({Kind::Blit, {{1, true, false, Stage::Single}}});
    for (unsigned i = 0; i < 40; i++)
      s.push_back({Kind::Compute, {{2, false, false, Stage::Single}}});
    s.push_back({Kind::Render, {{0, false, false, Stage::Fragment}, {1, false, false, Stage::Fragment}}});
    both("same-slot producers 256 apart", s, 3, false);
  }
  // scripted: a render-target chain with depth, interleaved with an independent upload stream
  {
    std::vector<Encoder> s;
    for (unsigned i = 0; i < 700; i++) {
      s.push_back({Kind::Render, {{0, true, false, Stage::Fragment}, {1, true, false, Stage::Fragment}}});
      s.push_back({Kind::Blit, {{2 + i % 5, true, false, Stage::Single}}});
      s.push_back({Kind::Render, {{2 + (i + 3) % 5, false, false, Stage::PreRaster}, {0, false, false, Stage::Fragment}}});
    }
    both("RT chain + uploads", s, 7, false);
  }

  // Fusion (checkEncoderRelation's render-into-render, through the production helper): what DEPS counts as
  // emitted after fusion drops waits on the fused pass's own updates, and keeps the external ones.
  {
    FenceLocalityCheck locality;
    locality.setCompressed(true);
    GenericAccessTracker rt, tex;
    EncoderBarrierState b;
    // a texture written by a blit, then two passes on the same target, both sampling it
    FenceSet w0;
    tex.accessExclusive(256, w0, b, false, kCompressedHorizon);
    auto blit_wait = locality.collectAndSimplifyWaits(w0, 256);
    FenceSet r0v, r0f, r1v, r1f;
    rt.accessExclusiveFragment(258, r0f, b, false, kCompressedHorizon);
    tex.accessSharedFragment(258, r0f, b, kCompressedHorizon);
    r0v = locality.collectAndSimplifyWaits(r0v, 257);
    r0f = locality.collectAndSimplifyWaits(r0f, 258, true);
    rt.accessExclusiveFragment(260, r1f, b, false, kCompressedHorizon);
    tex.accessSharedFragment(260, r1f, b, kCompressedHorizon);
    r1v = locality.collectAndSimplifyWaits(r1v, 259);
    r1f = locality.collectAndSimplifyWaits(r1f, 260, true);
    uint32_t before_fusion = emittedFences(r0f, &r0v) + emittedFences(r1f, &r1v);
    expect(r1f.test(258), "the second pass waits on the first (same target)");
    FenceSet r0u(258), r0uv(257), r1u(260), r1uv(259);
    fuseRenderPassFences(r1f, r1v, r1u, r1uv, r0f, r0v, r0u, r0uv);
    uint32_t after_fusion = emittedFences(r1f, &r1v);
    std::printf("fusion: waits before=%u after=%u (blit %u), updates after=%u\n", before_fusion, after_fusion,
                emittedFences(blit_wait), emittedFences(r1u, &r1uv));
    expect(!r1f.test(258) && !r1f.test(257), "fused pass still waits on its own fused update");
    expect(r1f.unionOf(r1v).test(256), "fused pass lost the external dependency on the blit");
    expect(after_fusion < before_fusion, "fusion removed no emitted wait");
    expect(emittedFences(r1u, &r1uv) == 4, "fused pass doesn't update all four stage fences");
  }

  // Random streams; in each pass the pre-raster accesses come first. The oracle has one node per stage, so
  // it can't see the intra-pass barrier that orders a later draw's pre-raster work after an earlier draw's
  // fragment work; streams with a fragment access before a pre-raster one would show false misses.
  // - Without pre-raster writes: both policies must be exact.
  // - With pre-raster writes (stream output, vertex-stage UAVs): the compressed policy must be exact. The
  //   legacy trackers miss a pass that writes at pre-raster and reads at fragment (a later writer waits for
  //   the pre-raster stage only, within 64 encoders); counted, not failed: upstream behaviour.
  uint64_t legacy_waits = 0, compressed_waits = 0, stages = 0, legacy_known = 0;
  unsigned seeds = 400;
  for (unsigned seed = 1; seed <= seeds; seed++) {
    unsigned resources = 4 + seed % 40;
    bool pre_raster_writes = seed % 2;
    auto s = randomStream(seed, 300 + (seed * 37) % 1500, resources, pre_raster_writes);
    auto legacy = run(s, resources, Policy::Legacy);
    auto compressed = run(s, resources, Policy::Compressed);
    uint64_t extra = 0;
    for (auto &m : compressed.missed)
      extra += !legacy.missed.count(m);
    if (extra || compressed.violations || (!pre_raster_writes && legacy.violations)) {
      std::printf("seed %u (pre-raster writes %d): legacy violations=%llu compressed violations=%llu, %llu only compressed\n",
                  seed, pre_raster_writes, (unsigned long long)legacy.violations,
                  (unsigned long long)compressed.violations, (unsigned long long)extra);
      expect(false, "random stream misses a dependency");
    }
    legacy_known += legacy.violations;
    legacy_waits += legacy.waits;
    compressed_waits += compressed.waits;
    stages += legacy.stages;
  }
  std::printf(
      "random: %u seeds, %llu stages: waits legacy=%llu (%.2f a stage) compressed=%llu (%.2f a stage); "
      "legacy's pre-raster-write misses=%llu (compressed: 0)\n", seeds,
      (unsigned long long)stages, (unsigned long long)legacy_waits, double(legacy_waits) / stages,
      (unsigned long long)compressed_waits, double(compressed_waits) / stages, (unsigned long long)legacy_known
  );
  // a control over the random streams too
  {
    auto s = randomStream(7, 1200, 30, false);
    auto control = run(s, 30, Policy::StrongOnly);
    std::printf("random control (strong waits only): violations=%llu\n", (unsigned long long)control.violations);
    expect(control.violations > 0, "random control found no violation");
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("dependency_compression_test: OK\n");
  return 0;
}
