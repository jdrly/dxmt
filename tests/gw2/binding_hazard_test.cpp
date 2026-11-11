// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O2 -Wall -Wextra -Isrc/dxmt -Isrc/util tests/gw2/binding_hazard_test.cpp -o $OUT
//
// gw2 (#147): the binding set's half of DXMT_SKIP_SAME_SRV (src/dxmt/dxmt_binding_set.hpp). No Metal,
// no Wine. The setters live in the context template, which needs the PE toolchain and Metal, so this
// drives the real BindingSet through the sequence the context runs for a texture bound as a shader
// resource and then as a render target. Two steps are copied here from
// src/d3d11/d3d11_context_impl.cpp: the skip's test (`sameBound`, SetShaderResource) and the unbind
// loop of ResolveSRVHazard (`resolve`, which OMSetRenderTargets runs for each new render target).
//
// - A slot first bound with a view that can be an output has its hazard bit; binding the texture as a
//   render target unbinds it, the skip's test no longer matches, and binding the view again replaces.
// - The hole: a slot that held a view that can't be an output keeps a clear bit when that view is
//   replaced by one that can. The render target's unbind misses it, and the skip's test would still
//   match while the texture is a render target. set_hazard (the switch's correction) closes it.

// the header relies on what its includers brought in before it
#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <utility>
using std::size_t;

#include "dxmt_binding_set.hpp"

#include <cstdio>

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

// SRV_B's shape and trait (src/d3d11/d3d11_context_state.hpp): the raw pointer decides redundancy.
struct Srv {
  void *RawPointer = nullptr;
  View *SRV = nullptr;
};

namespace dxmt {
template <> struct redundant_binding_trait<Srv> {
  static bool
  is_redundant(const Srv &left, const Srv &right) {
    return left.RawPointer == right.RawPointer;
  }
};
} // namespace dxmt

using Set = BindingSet<Srv, 128>;

// SetShaderResource: what the slot holds is the incoming view
static bool
sameBound(Set &set, unsigned slot, View *view) {
  return set.test_bound(slot) && set.at(slot).RawPointer == view;
}

// SetShaderResource's bind of a view that passed ValidateSRVHazard; `correct` is the switch on
static bool
bindView(Set &set, unsigned slot, View *view, bool correct) {
  bool replaced = false;
  bool missed = view->output && set.test_bound(slot) && !set.test_hazard(slot);
  auto &entry = set.bind(slot, {view}, replaced, view->output);
  if (!replaced)
    return false;
  if (missed && correct)
    set.set_hazard(slot);
  entry.SRV = view;
  return true;
}

// ResolveSRVHazard's loop for a render target on `resource` (CheckOverlap: the same resource)
static unsigned
resolve(Set &set, int resource) {
  unsigned unbound = 0;
  for (auto it = set.hazard_begin(); it != set.hazard_end(); it++) {
    const auto &[slot, bound] = *it;
    if (bound.SRV->resource == resource && set.unbind(slot))
      unbound++;
  }
  return unbound;
}

static void
firstBound() {
  for (bool correct : {false, true}) {
    Set set;
    View target{1, true};
    CHECK(bindView(set, 0, &target, correct));
    CHECK(set.test_hazard(0));
    CHECK(sameBound(set, 0, &target));
    CHECK(!bindView(set, 0, &target, correct)); // the same view again: redundant either way

    CHECK(resolve(set, 1) == 1); // bound as a render target
    CHECK(!set.test_bound(0));
    CHECK(!set.test_hazard(0));
    CHECK(set.at(0).RawPointer == nullptr);
    CHECK(!sameBound(set, 0, &target)); // the next PSSetShaderResources isn't skipped

    CHECK(bindView(set, 0, &target, correct)); // after the render target: bound again
    CHECK(set.test_bound(0) && set.test_hazard(0));
    CHECK(set.test_dirty(0));
  }
}

static void
replacedInBoundSlot() {
  View plain{2, false};
  View target{1, true};

  // Upstream's bit (the switch off): the replacement keeps the plain view's clear bit
  {
    Set set;
    CHECK(bindView(set, 0, &plain, false));
    CHECK(!set.test_hazard(0));
    CHECK(bindView(set, 0, &target, false));
    CHECK(!set.test_hazard(0));
    CHECK(resolve(set, 1) == 0);         // the render target's unbind misses the slot
    CHECK(sameBound(set, 0, &target));   // a skip here would keep it bound under the render target
  }

  // The switch on: the replacement sets the bit
  {
    Set set;
    CHECK(bindView(set, 0, &plain, true));
    CHECK(bindView(set, 0, &target, true));
    CHECK(set.test_hazard(0));
    CHECK(resolve(set, 1) == 1);
    CHECK(!sameBound(set, 0, &target));
    CHECK(bindView(set, 0, &target, true));
    CHECK(set.test_hazard(0));
  }

  // A view that can't be an output replacing one that can keeps the set bit: the unbind loop then
  // only compares resources, and another resource doesn't match
  {
    Set set;
    CHECK(bindView(set, 3, &target, true));
    CHECK(bindView(set, 3, &plain, true));
    CHECK(set.test_hazard(3));
    CHECK(resolve(set, 1) == 0);
    CHECK(sameBound(set, 3, &plain));
  }
}

int
main() {
  firstBound();
  replacedInBoundSlot();
  std::printf("binding_hazard: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
