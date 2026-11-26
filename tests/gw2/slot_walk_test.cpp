// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O2 -Wall -Wextra -Isrc/dxmt -Isrc/util tests/gw2/slot_walk_test.cpp -o $OUT
//
// gw2 (#203): DXMT_SKIP_EMPTY_SLOTS's walk (visit_settable_slots and BindingSet::bound_range in
// src/dxmt/dxmt_binding_set.hpp). No Metal, no Wine. The setters live in the context template, which
// needs the PE toolchain and Metal, so their per-slot bodies are copied here from
// src/d3d11/d3d11_context_impl.cpp (SetShaderResource and SetSamplers): the same tests, binds,
// unbinds, hazard bits and counters, with an emit recorded instead of a command.
//
// - bound_range returns test_bound's bits for every range of 1 to 64 slots, across the 64-slot word.
// - Random call sequences run through the full walk and the skipping one on two sets: ranges of 1 to
//   64 slots (and some of 0 or more than 64), null and non-null entries, the last call repeated or
//   partly replaced, draws (clear_dirty) and render targets (ResolveSRVHazard's unbind) in between.
//   After every step both sets emitted the same binds and unbinds in the same order, moved the same
//   counters, and hold the same views and the same bound, dirty and hazard bits.
// - The skipping walk visits only non-null or bound slots.
// - It prints both walks' ns a call on GW2's shape (32 slots, 3 non-null) and asserts nothing about
//   them: the runner's clock is too uneven for a threshold (#179).

// the header relies on what its includers brought in before it
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
using std::size_t;

#include "dxmt_binding_set.hpp"

#include <chrono>
#include <cstdio>
#include <random>
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

// A view of a resource; `output` is !hazardsFree() (the resource can be a render target).
struct View {
  int resource;
  bool output;
};

struct SamplerState {
  int id;
};

// SRV_B's and SAMPLER_B's shape and trait (src/d3d11/d3d11_context_state.hpp): the raw pointer
// decides redundancy.
struct Srv {
  void *RawPointer = nullptr;
  View *SRV = nullptr;
};
struct Smp {
  void *RawPointer = nullptr;
  SamplerState *Sampler = nullptr;
};

namespace dxmt {
template <> struct redundant_binding_trait<Srv> {
  static bool
  is_redundant(const Srv &left, const Srv &right) {
    return left.RawPointer == right.RawPointer;
  }
};
template <> struct redundant_binding_trait<Smp> {
  static bool
  is_redundant(const Smp &left, const Smp &right) {
    return left.RawPointer == right.RawPointer;
  }
};
} // namespace dxmt

using SrvSet = BindingSet<Srv, 128>;
using SmpSet = BindingSet<Smp, 16>;

// One emitted command: the slot and what it binds (null: an unbind)
struct Emit {
  unsigned slot;
  const void *what;
  bool
  operator==(const Emit &) const = default;
};

struct Counters {
  uint64_t same = 0, set = 0, missed = 0;
  bool
  operator==(const Counters &) const = default;
};

// One context's stage: its bindings, what it emitted, its counters, and which resources are bound
// as outputs (ValidateSRVHazard's answer for a view that can be an output)
struct Stage {
  SrvSet srvs;
  SmpSet samplers;
  std::vector<Emit> emits;
  Counters srv, smp;
  uint64_t visits = 0;
};

// SetShaderResource's walk and body (d3d11_context_impl.cpp), `skip` is DXMT_SKIP_EMPTY_SLOTS and
// `skipSame` DXMT_SKIP_SAME_SRV
static void
setShaderResources(
    Stage &st, unsigned StartSlot, unsigned NumViews, View *const *ppViews, bool skip, bool skipSame,
    const std::array<bool, 8> &outputBound
) {
  auto visit = [&](unsigned slot) {
    st.visits++;
    auto pView = ppViews[slot - StartSlot];
    if (pView && st.srvs.test_bound(slot) && !st.srvs.test_hazard(slot) && st.srvs.at(slot).RawPointer == pView) {
      st.srv.same++;
      if (skipSame)
        return;
    }
    if (pView && (!pView->output || !outputBound[pView->resource])) {
      bool replaced = false;
      bool hazard = pView->output;
      bool missed = hazard && st.srvs.test_bound(slot) && !st.srvs.test_hazard(slot);
      auto &entry = st.srvs.bind(slot, {pView}, replaced, hazard);
      if (!replaced)
        return;
      if (missed) {
        st.srv.missed++;
        if (skipSame)
          st.srvs.set_hazard(slot);
      }
      st.srv.set++;
      entry.SRV = pView;
      st.emits.push_back({slot, pView});
    } else {
      if (st.srvs.unbind(slot)) {
        st.srv.set++;
        st.emits.push_back({slot, nullptr});
      }
    }
  };
  visit_settable_slots(st.srvs, StartSlot, NumViews, ppViews, skip, visit);
}

// SetSamplers' walk and body
static void
setSamplers(Stage &st, unsigned StartSlot, unsigned NumSamplers, SamplerState *const *ppSamplers, bool skip) {
  auto visit = [&](unsigned Slot) {
    st.visits++;
    auto pSampler = ppSamplers[Slot - StartSlot];
    if (pSampler) {
      bool replaced = false;
      auto &entry = st.samplers.bind(Slot, {pSampler}, replaced);
      if (!replaced)
        return;
      st.smp.set++;
      entry.Sampler = pSampler;
      st.emits.push_back({100 + Slot, pSampler});
    } else {
      if (st.samplers.unbind(Slot)) {
        st.smp.set++;
        st.emits.push_back({100 + Slot, nullptr});
      }
    }
  };
  visit_settable_slots(st.samplers, StartSlot, NumSamplers, ppSamplers, skip, visit);
}

// ResolveSRVHazard's loop for a render target on `resource`
static void
resolve(Stage &st, int resource) {
  for (auto it = st.srvs.hazard_begin(); it != st.srvs.hazard_end(); it++) {
    const auto &[slot, bound] = *it;
    if (bound.SRV->resource == resource && st.srvs.unbind(slot))
      st.emits.push_back({unsigned(slot), nullptr});
  }
}

template <typename Set>
static bool
sameBits(const Set &a, const Set &b, size_t n) {
  for (size_t slot = 0; slot < n; slot++) {
    if (a.test_bound(slot) != b.test_bound(slot) || a.test_dirty(slot) != b.test_dirty(slot) ||
        a.test_hazard(slot) != b.test_hazard(slot))
      return false;
  }
  return true;
}

template <typename Set>
static bool
sameViews(Set &a, Set &b, size_t n) {
  for (size_t slot = 0; slot < n; slot++) {
    if (a.at(slot).RawPointer != b.at(slot).RawPointer)
      return false;
  }
  return true;
}

static void
boundRange() {
  std::mt19937_64 rng(203);
  for (int round = 0; round < 64; round++) {
    SrvSet srvs;
    SmpSet samplers;
    bool r = false;
    for (unsigned slot = 0; slot < 128; slot++) {
      if (rng() % 3 == 0)
        srvs.bind(slot, {&srvs}, r);
      if (slot < 16 && rng() % 3 == 0)
        samplers.bind(slot, {&samplers}, r);
    }
    bool ok = true;
    for (unsigned start = 0; start < 128; start++) {
      for (unsigned count = 1; count <= 64 && start + count <= 128; count++) {
        uint64_t want = 0;
        for (unsigned i = 0; i < count; i++)
          want |= uint64_t(srvs.test_bound(start + i)) << i;
        ok &= srvs.bound_range(start, count) == want;
        if (start + count <= 16) {
          want = 0;
          for (unsigned i = 0; i < count; i++)
            want |= uint64_t(samplers.test_bound(start + i)) << i;
          ok &= samplers.bound_range(start, count) == want;
        }
      }
    }
    CHECK(ok);
  }
}

// Random call sequences through both walks
static void
parity() {
  std::mt19937_64 rng(0x203);
  std::vector<View> views;
  for (int i = 0; i < 24; i++)
    views.push_back({i % 8, i % 3 == 0});
  std::vector<SamplerState> samplerPool(8);
  for (int i = 0; i < 8; i++)
    samplerPool[i].id = i;

  for (int run = 0; run < 200; run++) {
    bool skipSame = run % 2;
    Stage full, skip;
    std::array<bool, 8> outputBound = {};
    std::vector<View *> lastViews;
    unsigned lastStart = 0;
    std::vector<SamplerState *> lastSamplers;
    unsigned lastSamplerStart = 0;
    bool ok = true;

    for (int step = 0; step < 400 && ok; step++) {
      unsigned op = rng() % 16;
      if (op < 9) {
        // XXSetShaderResources: new, the last call again, or the last call with a few entries replaced
        unsigned kind = rng() % 4;
        if (kind == 0 || lastViews.empty()) {
          unsigned count = rng() % 20 == 0 ? (rng() % 2 ? 0 : 65 + rng() % 40) : 1 + rng() % 64;
          lastStart = rng() % (128 - count + 1);
          unsigned nonNull = rng() % 4; // 0: all null, 1: few, 2: half, 3: most
          lastViews.assign(count, nullptr);
          for (auto &v : lastViews) {
            unsigned roll = rng() % 8;
            bool set = nonNull == 1 ? roll == 0 : nonNull == 2 ? roll < 4 : nonNull == 3 ? roll < 7 : false;
            v = set ? &views[rng() % views.size()] : nullptr;
          }
        } else if (kind == 1) {
          for (unsigned i = 0, n = 1 + rng() % 3; i < n && !lastViews.empty(); i++) {
            unsigned roll = rng() % 3;
            lastViews[rng() % lastViews.size()] = roll == 0 ? nullptr : &views[rng() % views.size()];
          }
        }
        setShaderResources(full, lastStart, lastViews.size(), lastViews.data(), false, skipSame, outputBound);
        setShaderResources(skip, lastStart, lastViews.size(), lastViews.data(), true, skipSame, outputBound);
      } else if (op < 13) {
        // XXSetSamplers
        if (rng() % 3 == 0 || lastSamplers.empty()) {
          unsigned count = rng() % 20 == 0 ? 0 : 1 + rng() % 16;
          lastSamplerStart = rng() % (16 - count + 1);
          lastSamplers.assign(count, nullptr);
          for (auto &s : lastSamplers)
            s = rng() % 3 ? nullptr : &samplerPool[rng() % samplerPool.size()];
        } else if (!lastSamplers.empty()) {
          lastSamplers[rng() % lastSamplers.size()] = rng() % 2 ? nullptr : &samplerPool[rng() % samplerPool.size()];
        }
        setSamplers(full, lastSamplerStart, lastSamplers.size(), lastSamplers.data(), false);
        setSamplers(skip, lastSamplerStart, lastSamplers.size(), lastSamplers.data(), true);
      } else if (op < 15) {
        // a draw: the encode side took the dirty slots
        full.srvs.clear_dirty();
        skip.srvs.clear_dirty();
        full.samplers.clear_dirty();
        skip.samplers.clear_dirty();
      } else {
        // a render target bound on one resource (another one leaves), unbinding its views
        int resource = rng() % 8;
        outputBound = {};
        outputBound[resource] = true;
        resolve(full, resource);
        resolve(skip, resource);
      }

      ok &= full.emits == skip.emits;
      ok &= full.srv == skip.srv && full.smp == skip.smp;
      ok &= sameBits(full.srvs, skip.srvs, 128) && sameBits(full.samplers, skip.samplers, 16);
      ok &= sameViews(full.srvs, skip.srvs, 128) && sameViews(full.samplers, skip.samplers, 16);
      if (!ok)
        std::printf("run %d diverged at step %d (op %u)\n", run, step, op);
    }
    CHECK(ok);
    CHECK(skip.visits <= full.visits);
  }
}

// Which slots the skipping walk visits
static void
visits() {
  Stage st;
  View a{0, false}, b{1, false}, c{2, true};
  std::array<bool, 8> none = {};
  std::array<View *, 32> call = {};
  call[0] = &a;
  call[3] = &b;
  call[7] = &c;

  setShaderResources(st, 0, 32, call.data(), true, false, none);
  CHECK(st.visits == 3); // the three non-null entries
  CHECK(st.emits.size() == 3);

  st.visits = 0;
  call[3] = nullptr;
  setShaderResources(st, 0, 32, call.data(), true, false, none);
  CHECK(st.visits == 3); // slot 3 is bound: visited, and unbound
  CHECK(st.emits.size() == 4 && st.emits.back() == Emit{3, nullptr});

  st.visits = 0;
  std::array<View *, 32> empty = {};
  setShaderResources(st, 64, 32, empty.data(), true, false, none);
  CHECK(st.visits == 0); // nothing bound there, nothing incoming

  st.visits = 0;
  setShaderResources(st, 40, 32, empty.data(), true, false, none);
  CHECK(st.visits == 0);

  st.visits = 0;
  setShaderResources(st, 0, 32, empty.data(), false, false, none);
  CHECK(st.visits == 32); // the switch off: every slot
}

// GW2's shape: 32 slots, three non-null, mostly the same views again
static void
bench() {
  View v[3] = {{0, false}, {1, false}, {2, false}};
  std::array<View *, 32> call = {};
  call[0] = &v[0];
  call[1] = &v[1];
  call[5] = &v[2];
  std::array<bool, 8> none = {};
  constexpr int kCalls = 200000;
  for (bool skip : {false, true}) {
    Stage st;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalls; i++) {
      call[5] = &v[2 - (i & 1)]; // one replacement a call
      setShaderResources(st, 0, 32, call.data(), skip, false, none);
      st.emits.clear();
    }
    auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench: %s walk, 32 slots, 3 non-null: %.1f ns a call (%llu visits)\n", skip ? "skipping" : "full",
                ns / kCalls, (unsigned long long)st.visits);
  }
}

int
main() {
  boundRange();
  parity();
  visits();
  bench();
  std::printf("slot_walk: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
