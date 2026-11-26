// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O2 -Wall -Wextra -Isrc/dxmt -Isrc/util tests/gw2/dependency_compression_test.cpp src/dxmt/dxmt_deptrack.cpp -o $OUT
//
// gw2 (#132): the encoder dependency policies (src/dxmt/dxmt_deptrack.*) against an independent hazard
// oracle. No Metal, no Wine.
//
// gw2 (#353): presents (src/dxmt/dxmt_gw2_present_fences.hpp). A present is an encoder of the chain; with
// DXMT_MTL4_PRESENT=mtl3 its pass runs in a Metal 3 buffer after every commit before it (the `frame`
// event), and the Metal 4 queue waits for its `shown` event before the next frame's first back-buffer
// writer (DXMT_MTL4_SHOWN_WAIT=write). The oracle models three ways its fences go: the Metal 4 present
// pass's (they reach the queue), mtl3's (they never do: a wait on its slot covers only that slot's older
// updates), and DXMT_MTL4_PRESENT_FENCES=1's (an encoder on the queue waits on the present's waits and
// updates its slot). A present that lands on a checkpoint with mtl3's fences must show violations, the
// other two none. Random frames show the other way in (the checkpoint whose window covers the present),
// and PresentFences must count a stale wait in every stream with a violation.
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
#include "dxmt_gw2_present_fences.hpp"

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

// gw2 (#353): Present (one stage; its pass reads the back buffer), ShownWait (no encoder: the Metal 4
// queue's wait for the latest present's `shown`)
enum class Kind { Compute, Blit, Render, Present, ShownWait };
// gw2 (#353): how a present's fences go (see the header)
enum class PresentMode { Metal4, Mtl3, Mtl3Fences };
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
  uint64_t stale = 0;              // gw2 (#353): PresentFences' stale waits
  uint64_t present_checkpoints = 0; // gw2 (#353): presents that were checkpoints
};

// The oracle's record of one resource's accesses: node = encoder stage ID, plus the encoder it belongs to
struct Touch {
  EncoderId node;
  uint64_t encoder;
};

bool verbose = false;

Result
run(const std::vector<Encoder> &stream, unsigned resources, Policy policy, PresentMode present_mode = PresentMode::Metal4) {
  FenceLocalityCheck locality;
  const bool mtl3 = present_mode != PresentMode::Metal4;
  gw2::PresentFences gap(present_mode == PresentMode::Mtl3Fences);
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
  // gw2 (#353): what every stage after the latest shown wait runs after (that present and its ancestors)
  Bits floor(max_id + 1);
  EncoderId last_present = 0;

  for (uint64_t e = 0; e < stream.size(); e++) {
    auto &enc = stream[e];
    if (enc.kind == Kind::ShownWait) {
      if (!mtl3)
        continue; // the Metal 4 present has no `shown` event
      if (last_present) {
        floor.merge(before[last_present]);
        floor.set(last_present);
      }
      gap.shownWaited(0, 0);
      continue;
    }
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
    const bool present = enc.kind == Kind::Present;
    if (present && policy != Policy::StrongOnly && locality.lastWasCheckpoint())
      r.present_checkpoints++;
    // gw2 (#353): the encode thread's count, on what the encoder emits
    if (!present && gap.encoder(id, emit, vid ? &emit_vertex : nullptr))
      r.stale++;

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
    // gw2 (#353): after a shown wait, everything runs after that present's Metal 3 buffer
    before[id].merge(floor);
    if (vid)
      before[vid].merge(floor);
    auto update = [&](EncoderId node) {
      auto &h = slot_history[node % kParityLane];
      h.set(node);
      h.merge(before[node]);
    };
    if (present && mtl3) {
      // gw2 (#353): the Metal 3 buffer runs after every commit before it (the `frame` event)
      Bits waited = before[id]; // with Mtl3Fences: what the fence-only encoder waited for
      for (EncoderId n = kParityLane; n < id; n++)
        before[id].set(n);
      if (present_mode == PresentMode::Mtl3Fences)
        slot_history[id % kParityLane].merge(waited); // the encoder's update: the waits done, not the pass
      gap.presented(id, locality.lastWasCheckpoint(), true, 0);
      last_present = id;
    } else {
      if (vid)
        update(vid);
      update(id);
      if (present) {
        gap.presented(id, locality.lastWasCheckpoint(), false, 0);
        last_present = id;
      }
    }

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

// gw2 (#353): frames as GW2 draws them: a random body over resources 1.., then the queue's shown wait,
// UI passes into the back buffer (resource 0), and the present that reads it
std::vector<Encoder>
frameStream(unsigned seed, unsigned frames, unsigned resources) {
  std::mt19937 rng(seed * 7919 + 1);
  auto body = randomStream(seed, frames * 90, resources - 1, seed % 2);
  for (auto &enc : body)
    for (auto &a : enc.accesses)
      a.resource++;
  std::vector<Encoder> s;
  size_t at = 0;
  unsigned first = resources - 1; // randomStream's first writes of every resource
  for (unsigned i = 0; i < first; i++)
    s.push_back(body[at++]);
  for (unsigned f = 0; f < frames && at < body.size(); f++) {
    unsigned n = 60 + rng() % 60;
    for (unsigned i = 0; i < n && at < body.size(); i++)
      s.push_back(body[at++]);
    s.push_back({Kind::ShownWait, {}});
    unsigned ui = 1 + rng() % 3;
    for (unsigned i = 0; i < ui; i++)
      s.push_back({Kind::Render, {{0, true, false, Stage::Fragment}, {1 + unsigned(rng() % (resources - 1)), false, false, Stage::Fragment}}});
    s.push_back({Kind::Present, {{0, false, false, Stage::Single}}});
    if (f == 0)
      s.push_back({Kind::ShownWait, {}}); // a context's first present waits for its `shown` at once
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
  // gw2 (#353): scripted presents. IDs from 256, one stage each, so the checkpoints are 256, 319, 382, 445
  // and 508. Each stream starts with a primed present (257, the back buffer written at 256): a context's
  // first present waits for its `shown` right after it, as production does, so the gap opens only at the
  // second one. Filler encoders write resource 2.
  auto modes = [](const std::vector<Encoder> &s, unsigned resources) {
    struct Modes {
      Result metal4, mtl3, fences;
    };
    return Modes{run(s, resources, Policy::Compressed, PresentMode::Metal4),
                 run(s, resources, Policy::Compressed, PresentMode::Mtl3),
                 run(s, resources, Policy::Compressed, PresentMode::Mtl3Fences)};
  };
  auto primed = [] {
    std::vector<Encoder> s;
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});    // 256: the back buffer
    s.push_back({Kind::Present, {{0, false, false, Stage::Single}}}); // 257
    s.push_back({Kind::ShownWait, {}});
    return s;
  };
  // fillers until the next encoder's ID is `next_id`
  auto fill_to = [](std::vector<Encoder> &s, unsigned next_id, unsigned reader_at = 0) {
    while (256 + s.size() - 1 < next_id) { // the ShownWait marker has no ID
      unsigned id = 256 + unsigned(s.size()) - 1;
      if (id == reader_at)
        s.push_back({Kind::Compute, {{1, false, false, Stage::Single}}});
      else
        s.push_back({Kind::Compute, {{2, true, false, Stage::Single}}});
    }
  };
  // a present on a checkpoint (382, window 256..318): the next frame's first encoder writes resource 1,
  // which encoder 300 read. Only the present's fences could order it.
  {
    auto s = primed();
    s.push_back({Kind::Blit, {{1, true, false, Stage::Single}}}); // 258 writes resource 1
    fill_to(s, 381, 300);                                         // 300 reads it
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});    // 381: the back buffer
    s.push_back({Kind::Present, {{0, false, false, Stage::Single}}}); // 382
    s.push_back({Kind::Blit, {{1, true, false, Stage::Single}}});    // 383: the next frame overwrites resource 1
    s.push_back({Kind::ShownWait, {}});
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});
    auto m = modes(s, 3);
    std::printf("present on a checkpoint (382): present checkpoints=%llu violations metal4=%llu mtl3=%llu "
                "mtl3+fences=%llu, stale waits mtl3=%llu mtl3+fences=%llu\n",
                (unsigned long long)m.mtl3.present_checkpoints, (unsigned long long)m.metal4.violations,
                (unsigned long long)m.mtl3.violations, (unsigned long long)m.fences.violations,
                (unsigned long long)m.mtl3.stale, (unsigned long long)m.fences.stale);
    expect(m.mtl3.present_checkpoints == 1, "the scripted present isn't a checkpoint");
    expect(m.metal4.violations == 0, "the Metal 4 present's fences leave a hazard");
    expect(m.mtl3.violations > 0, "mtl3's unreached present fence leaves no hazard: the case doesn't show the hole");
    expect(m.mtl3.stale > 0, "PresentFences counts no stale wait where the hazard is");
    expect(m.fences.violations == 0, "DXMT_MTL4_PRESENT_FENCES=1 leaves a hazard");
    expect(m.fences.stale == 0, "DXMT_MTL4_PRESENT_FENCES=1 still counts stale waits");
  }
  // the checkpoint that covers a present (444, not a checkpoint; 508's window is 382..444). The pass
  // before it (443) reads resource 3 and writes the back buffer. 508 waits on the present's slot and
  // drops 443 through the present's summary; 509 overwrites resource 3 and waits on 508 only.
  {
    auto s = primed();
    fill_to(s, 443); // the next ID is 443
    s.push_back({Kind::Compute, {{3, false, false, Stage::Single}, {0, true, false, Stage::Single}}}); // 443
    s.push_back({Kind::Present, {{0, false, false, Stage::Single}}}); // 444
    fill_to(s, 509); // 445..508, the covering checkpoint included
    s.push_back({Kind::Blit, {{3, true, false, Stage::Single}}}); // 509
    s.push_back({Kind::ShownWait, {}});
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});
    auto m = modes(s, 4);
    std::printf("the checkpoint covering a present (444, 508): present checkpoints=%llu violations metal4=%llu "
                "mtl3=%llu mtl3+fences=%llu, stale waits mtl3=%llu\n",
                (unsigned long long)m.mtl3.present_checkpoints, (unsigned long long)m.metal4.violations,
                (unsigned long long)m.mtl3.violations, (unsigned long long)m.fences.violations,
                (unsigned long long)m.mtl3.stale);
    expect(m.mtl3.present_checkpoints == 0, "the covered present is a checkpoint");
    expect(m.metal4.violations == 0, "the Metal 4 present's fences leave a hazard (covering checkpoint)");
    expect(m.mtl3.violations > 0, "the covering checkpoint's drop through the present leaves no hazard");
    expect(m.mtl3.stale > 0, "PresentFences counts no stale wait at the covering checkpoint");
    expect(m.fences.violations == 0, "DXMT_MTL4_PRESENT_FENCES=1 leaves a hazard (covering checkpoint)");
  }
  // a present one after a checkpoint, covered by nothing before the next frame's shown wait
  {
    auto s = primed();
    s.push_back({Kind::Blit, {{1, true, false, Stage::Single}}});
    fill_to(s, 383, 300);
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});    // 383
    s.push_back({Kind::Present, {{0, false, false, Stage::Single}}}); // 384
    s.push_back({Kind::Blit, {{1, true, false, Stage::Single}}});
    s.push_back({Kind::ShownWait, {}});
    s.push_back({Kind::Blit, {{0, true, false, Stage::Single}}});
    auto m = modes(s, 3);
    std::printf("present after a checkpoint (384): violations mtl3=%llu stale=%llu\n",
                (unsigned long long)m.mtl3.violations, (unsigned long long)m.mtl3.stale);
    expect(m.mtl3.present_checkpoints == 0, "the present after the checkpoint is one");
    expect(m.mtl3.violations == 0, "a present that isn't a checkpoint, with nothing covering it, leaves a hazard");
  }
  // PresentFences: an encoder without a fence ID, and a second unreached present before a shown wait
  {
    gw2::PresentFences fences(false);
    fences.begin(8);
    fences.presented(300, false, true, 1);
    FenceSet on_first(300);
    expect(!fences.encoder(~0ull, on_first), "an encoder without a fence ID counts as a stale wait");
    expect(fences.encoder(310, on_first), "a timestamp encoder dropped the outstanding present");
    fences.presented(320, false, true, 2);
    expect(fences.encoder(330, on_first), "a second present dropped the first one's gap");
    FenceSet on_both(300);
    on_both.set(320);
    fences.encoder(332, on_both);
    expect(fences.chunk().stale_present == 320, "the record doesn't name the latest present waited on");
    expect(fences.encoder(331, FenceSet(320)), "the second present's gap isn't followed");
    fences.shownWaited(5, 2);
    expect(!fences.encoder(340, on_first) && !fences.encoder(341, FenceSet(320)), "a shown wait left a gap");
  }
  // gw2 (#353): random frames, both policies. The Metal 4 present and DXMT_MTL4_PRESENT_FENCES=1 must be
  // exact; mtl3's fences are counted. Wherever mtl3 misses a dependency, PresentFences counted a stale wait.
  {
    uint64_t frames_total = 0, cps = 0, missed[2] = {}, stale[2] = {}, seeds_missed[2] = {};
    for (unsigned seed = 1; seed <= 120; seed++) {
      unsigned resources = 6 + seed % 30;
      auto s = frameStream(seed, 12, resources);
      frames_total += 12;
      for (int p = 0; p < 2; p++) {
        Policy policy = p ? Policy::Compressed : Policy::Legacy;
        auto metal4 = run(s, resources, policy, PresentMode::Metal4);
        auto mtl3 = run(s, resources, policy, PresentMode::Mtl3);
        auto fences = run(s, resources, policy, PresentMode::Mtl3Fences);
        // the legacy trackers' pre-raster-write misses (above) show in every mode, except where mtl3's
        // `shown` wait happens to order them: compare with Metal 4's
        bool subset = true;
        for (auto &m : fences.missed)
          subset &= metal4.missed.count(m) > 0;
        expect(subset, "DXMT_MTL4_PRESENT_FENCES=1 misses what the Metal 4 present doesn't");
        expect(fences.stale == 0, "DXMT_MTL4_PRESENT_FENCES=1 counts stale waits");
        if (p)
          expect(metal4.violations == 0, "the compressed policy misses a dependency with Metal 4 presents");
        uint64_t extra = 0;
        for (auto &m : mtl3.missed)
          extra += !metal4.missed.count(m);
        missed[p] += extra;
        stale[p] += mtl3.stale;
        seeds_missed[p] += extra > 0;
        if (extra && !mtl3.stale) {
          std::printf("seed %u (%s): mtl3 misses %llu with no stale wait counted\n", seed, p ? "compressed" : "legacy",
                      (unsigned long long)extra);
          expect(false, "a miss through an unreached present fence that PresentFences didn't count");
        }
        if (p)
          cps += mtl3.present_checkpoints;
      }
    }
    std::printf("present frames: %llu frames, %llu presents on a checkpoint (compressed); mtl3 misses: compressed=%llu "
                "in %llu of 120 seeds (stale waits %llu), legacy=%llu in %llu (stale waits %llu); "
                "the Metal 4 present and mtl3+fences: none\n",
                (unsigned long long)frames_total, (unsigned long long)cps, (unsigned long long)missed[1],
                (unsigned long long)seeds_missed[1], (unsigned long long)stale[1], (unsigned long long)missed[0],
                (unsigned long long)seeds_missed[0], (unsigned long long)stale[0]);
    expect(missed[1] > 0, "random frames never showed the compressed policy's hole through a present");
  }

  // gw2 (#353): the finish thread's lines: an error chunk with the one before it, then the chunk after it
  {
    gw2::PresentFences fences(false);
    gw2::PresentFenceReport report;
    FenceSet none, on_present(19374);
    fences.begin(44);
    fences.commitBegan(0);
    fences.presented(19374, true, true, 3109); // chunk 3121's present: a checkpoint, unreached
    auto c3121 = fences.chunk();
    fences.begin(45);
    fences.commitBegan(0);
    expect(fences.encoder(19380, on_present), "a wait on the unreached present isn't counted");
    expect(!fences.encoder(19381, none), "a wait elsewhere is counted");
    fences.shownWaited(42, 3109);
    fences.commitBegan(42);
    expect(!fences.encoder(19400, on_present), "a wait after the shown wait is counted");
    fences.presented(19420, false, true, 3110);
    auto c3122 = fences.chunk();
    expect(report.completed(3121, 3109, c3121, false, "0").empty(), "a chunk without an error gets a line");
    auto lines = report.completed(3122, 3110, c3122, true, "0");
    auto after = report.completed(3123, 3111, c3122, false, "0");
    for (auto &l : lines)
      std::printf("%s\n", l.c_str());
    for (auto &l : after)
      std::printf("%s\n", l.c_str());
    expect(lines.size() == 1 && after.size() == 1, "an error chunk and the one after it don't get one line each");
    expect(lines.size() == 1 &&
               lines[0].find("chunk 3122 (frame 3110): encoders=45 commits=0,42 shown=42:3109 present=19420:mtl3,unreached "
                             "frame=3110 stale=1:19374; the chunk before, chunk 3121 (frame 3109): encoders=44 commits=0 "
                             "shown=- present=19374:mtl3,checkpoint,unreached frame=3109 stale=0") != std::string::npos,
           "the error line's records");
    expect(after.size() == 1 && after[0].find("after the device error in chunk 3122: chunk 3123") != std::string::npos,
           "the line after names the error chunk");
    gw2::PresentFences emitting(true);
    emitting.begin(1);
    expect(!emitting.presented(19374, true, true, 1), "DXMT_MTL4_PRESENT_FENCES=1 leaves the present unreached");
    expect(!emitting.encoder(19380, on_present), "DXMT_MTL4_PRESENT_FENCES=1 counts a stale wait");
  }

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
