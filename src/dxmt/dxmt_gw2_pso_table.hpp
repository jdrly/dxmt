#pragma once

// gw2 (#366): DXMT_PSO_TABLE: a context's recent plain pipelines in a table of 64 slots indexed by the key's hash,
// in place of DXMT_PSO_MRU's 4 entries compared in turn (src/d3d11/d3d11_context_impl.cpp, LookupPipelineMru). A
// slot holds one key, its full hash and its pipeline; a key whose slot holds another one replaces it. A hit needs
// the slot's hash and then the cache's own equality, so it returns the pipeline the device's cache would. The
// hash is std::hash of the key, the one the device's cache computes, blend fingerprint (DXMT_PSO_BLEND_HASH)
// included.
//
// Lifetime and threads as the MRU's: the keys' pointers and the pipelines belong to the device's caches, which
// never evict, so the table holds no references; the context touches it only under its lock.
//
// Pure C++ (no Windows, no Metal): tests/gw2/pso_table_test.cpp builds it natively.

#include <cstddef>
#include <functional>

namespace dxmt::gw2 {

template <typename Key, typename Value, size_t N = 64, typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class PsoTable {
public:
  // The value of an equal key, or false. Remembers the probed slot for insert(). An empty slot is never compared,
  // since Equal may dereference the key's pointers.
  bool
  find(const Key &key, Value &value) {
    hash_ = Hash{}(key);
    Slot &slot = slots_[hash_ % N];
    if (!slot.used || slot.hash != hash_ || !Equal{}(slot.key, key))
      return false;
    value = slot.value;
    return true;
  }

  // After a find() of `key` that missed: `key` and its value take the slot that find() probed.
  void
  insert(const Key &key, Value value) {
    slots_[hash_ % N] = {true, hash_, key, value};
  }

private:
  struct Slot {
    bool used;
    size_t hash;
    Key key;
    Value value;
  };
  Slot slots_[N] = {};
  size_t hash_ = 0;
};

} // namespace dxmt::gw2
