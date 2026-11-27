// gw2-test: pure
// gw2-build: clang++ -std=c++20 -Wall -Wextra -Isrc/dxmt tests/gw2/pso_table_test.cpp -o $OUT
// gw2-run: "$OUT" on
// gw2-run: "$OUT" off
//
// gw2 (#366): a context's recent pipelines, DXMT_PSO_TABLE on (the table, src/dxmt/dxmt_gw2_pso_table.hpp) and
// off (DXMT_PSO_MRU's 4 entries). The context isn't built here (it needs the PE toolchain and Metal), so its
// draw-path steps are copied from src/d3d11/d3d11_context_impl.cpp (FinalizeCurrentRenderPipeline,
// LookupPipelineMru, InsertPipelineMru): look up, else ask the device and insert. The key's hash is a field of
// the key, so the test picks the slots; its equality dereferences a pointer, as the pipeline key's does its
// blend state, so a compare with an empty slot crashes. Each draw must get the pipeline the device holds for
// its key. Then, both ways:
//
// - sameSlot: two keys 64 apart in hash take one slot (on: each evicts the other), and two keys with one full
//   hash are told apart by the equality.
// - rebuilt: a key's slot is taken, the device has a new pipeline for it, and the key comes back: it gets the
//   new one, then hits it.
// - many: 64 keys of distinct slots, and 100 keys (36 slots shared), each cycled twice.
//
// Run: tests/gw2/run.zsh pure, or the commands above with $OUT set.

#include "dxmt_gw2_pso_table.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// A stand-in for MTL_GRAPHICS_PIPELINE_DESC: its hash is chosen, and its equality reads through a pointer.
struct Key {
  const int *id;
  size_t hash;
};
struct KeyHash {
  size_t
  operator()(const Key &k) const {
    return k.hash;
  }
};
struct KeyEqual {
  bool
  operator()(const Key &x, const Key &y) const {
    return *x.id == *y.id;
  }
};
using Pipeline = uintptr_t;

// The device's pipeline cache: one pipeline per key id, which rebuild() replaces.
struct Device {
  std::map<int, Pipeline> pipelines;
  Pipeline next = 1;
  unsigned lookups = 0;
  Pipeline
  create(const Key &k) {
    lookups++;
    auto [it, added] = pipelines.try_emplace(*k.id, next);
    if (added)
      next++;
    return it->second;
  }
  void
  rebuild(int id) {
    pipelines[id] = next++;
  }
};

// The context's lookup, as FinalizeCurrentRenderPipeline does it with DXMT_PSO_MRU on.
struct Context {
  Context(bool on, Device &d) : table_on(on), device(d) {}
  bool table_on;
  Device &device;
  unsigned hits = 0;

  dxmt::gw2::PsoTable<Key, Pipeline, 64, KeyHash, KeyEqual> table;

  // LookupPipelineMru and InsertPipelineMru's 4 entries
  static constexpr uint32_t kPsoMruSize = 4;
  struct PsoMruEntry {
    Key desc;
    Pipeline pipeline;
  };
  PsoMruEntry pso_mru_[kPsoMruSize] = {};
  uint32_t pso_mru_count_ = 0;
  uint32_t pso_mru_next_ = 0;

  bool
  lookupMru(const Key &desc, Pipeline *ppPipeline) {
    if (table_on)
      return table.find(desc, *ppPipeline);
    KeyEqual equal;
    for (uint32_t i = 0; i < pso_mru_count_; i++) {
      auto &entry = pso_mru_[(pso_mru_next_ + kPsoMruSize - 1 - i) % kPsoMruSize];
      if (equal(entry.desc, desc)) {
        *ppPipeline = entry.pipeline;
        return true;
      }
    }
    return false;
  }

  void
  insertMru(const Key &desc, Pipeline pipeline) {
    if (table_on) {
      table.insert(desc, pipeline);
      return;
    }
    pso_mru_[pso_mru_next_] = {desc, pipeline};
    pso_mru_next_ = (pso_mru_next_ + 1) % kPsoMruSize;
    pso_mru_count_ = std::min(pso_mru_count_ + 1, kPsoMruSize);
  }

  // One draw after a pipeline change. Returns whether it hit; checks it got the device's pipeline.
  bool
  draw(const Key &desc) {
    Pipeline pipeline;
    bool hit = lookupMru(desc, &pipeline);
    if (hit)
      hits++;
    else {
      pipeline = device.create(desc);
      insertMru(desc, pipeline);
    }
    CHECK(pipeline == device.pipelines.at(*desc.id));
    return hit;
  }
};

static int ids[200];

static Key
key(int id, size_t hash) {
  return {&ids[id], hash};
}

static void
sameSlot(bool on) {
  Device device;
  Context ctx(on, device);
  Key a = key(1, 5), b = key(2, 5 + 64), c = key(3, 7), d = key(4, 7);

  CHECK(!ctx.draw(a)); // the empty table: no compare
  CHECK(ctx.draw(a));
  CHECK(!ctx.draw(b)); // a's slot (on), a new entry (off)
  CHECK(ctx.draw(a) == !on);
  CHECK(ctx.draw(a)); // a has its slot back (on)
  CHECK(!ctx.draw(c));
  CHECK(!ctx.draw(d)); // c's full hash, but not equal: the equality rejects it
  CHECK(ctx.draw(d));
  CHECK(ctx.draw(c) == !on);
  std::printf("%s: sameSlot hits=%u lookups=%u\n", on ? "on" : "off", ctx.hits, device.lookups);
}

static void
rebuilt(bool on) {
  Device device;
  Context ctx(on, device);
  Key a = key(1, 9);
  CHECK(!ctx.draw(a));
  CHECK(ctx.draw(a));
  // a leaves: its slot (on), or the 4 entries (off)
  if (on)
    CHECK(!ctx.draw(key(2, 9 + 64)));
  else
    for (int i = 2; i < 6; i++)
      CHECK(!ctx.draw(key(i, 9 + i)));
  Pipeline old = device.pipelines.at(1);
  device.rebuild(1);
  CHECK(device.pipelines.at(1) != old);
  CHECK(!ctx.draw(a)); // the new pipeline (draw checks it), not the old one
  CHECK(ctx.draw(a));
  std::printf("%s: rebuilt hits=%u lookups=%u\n", on ? "on" : "off", ctx.hits, device.lookups);
}

static void
many(bool on, int keys) {
  Device device;
  Context ctx(on, device);
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < keys; i++)
      ctx.draw(key(i, size_t(i)));
  // on: the second pass hits every key alone in its slot (slots keys-64 to 63), none of the ones that share
  // a slot. off: 4 entries never hold a key a cycle of 64 or 100 comes back to.
  unsigned want = !on ? 0 : keys <= 64 ? unsigned(keys) : unsigned(64 - (keys - 64));
  CHECK(ctx.hits == want);
  CHECK(device.lookups == unsigned(2 * keys) - ctx.hits);
  std::printf("%s: many keys=%d hits=%u lookups=%u\n", on ? "on" : "off", keys, ctx.hits, device.lookups);
}

int
main(int argc, char **argv) {
  if (argc != 2 || (std::strcmp(argv[1], "on") && std::strcmp(argv[1], "off"))) {
    std::printf("usage: %s on|off\n", argv[0]);
    return 2;
  }
  bool on = std::strcmp(argv[1], "on") == 0;
  for (int i = 0; i < 200; i++)
    ids[i] = i;

  sameSlot(on);
  rebuilt(on);
  many(on, 64);
  many(on, 100);

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
