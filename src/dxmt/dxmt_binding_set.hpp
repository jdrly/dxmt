#pragma once

#include "util_bit.hpp"

namespace dxmt {

template <typename E> struct redundant_binding_trait {
  static bool is_redundant(const E &left, const E &right);
};

/**
Element is required to be move-assignable
 */
template <typename Element, size_t NumElements> class BindingSet {
  bit::bitset<NumElements> dirty;
  bit::bitset<NumElements> bound;
  bit::bitset<NumElements> hazard;
  std::array<Element, NumElements> storage;

public:
  BindingSet() {};
  BindingSet(const BindingSet &copy) = delete; // doesn't make sense here
  BindingSet(BindingSet &&move) : storage(std::move(move.storage)) {
    bound = move.bound;
    dirty = move.bound; // intended behavior
    hazard = move.hazard;
  };

  BindingSet &
  operator=(BindingSet &&move) {
    storage = std::move(move.storage);
    bound = move.bound;
    dirty = move.bound; // intended behavior
    hazard = move.hazard;
    return *this;
  }

  constexpr Element &
  at(uint64_t index) noexcept {
    return storage[index];
  };

  constexpr Element &
  operator[](uint64_t index) noexcept {
    assert(bound.get(index));
    return storage[index];
  };

  constexpr bool
  test_bound(size_t slot) const noexcept {
    return bound.get(slot);
  };

  // gw2 (#147): bind() sets a slot's hazard bit only when the slot goes from unbound to bound; a
  // replacement in a bound slot keeps the old bit. These let a caller read and correct it.
  constexpr bool
  test_hazard(size_t slot) const noexcept {
    return hazard.get(slot);
  };

  inline void
  set_hazard(size_t slot) {
    hazard.set(slot, true);
  };

  constexpr bool
  test_dirty(size_t slot) const noexcept {
    return dirty.get(slot);
  };

  constexpr bool
  any_dirty() const noexcept {
    return dirty.any();
  }

  constexpr bool
  any_dirty_masked(uint16_t mask) const noexcept {
    return (dirty.qword(0) & (uint64_t)mask) != 0;
  }

  constexpr bool
  any_dirty_masked(uint64_t mask) const noexcept {
    return (dirty.qword(0) & mask) != 0;
  }

  constexpr bool
  any_dirty_masked(uint64_t mask_hi, uint64_t mask_lo) const noexcept {
    return ((dirty.qword(0) & mask_lo) | (dirty.qword(1) & mask_hi)) != 0;
  }

  constexpr bool
  all_bound_masked(uint32_t mask) const noexcept {
    return (bound.qword(0) & mask) == mask;
  }

  constexpr bool
  any_bound() const noexcept {
    return bound.any();
  }

  constexpr bool
  any_bound_masked(uint64_t mask) const noexcept {
    return (bound.qword(0) & mask) != 0;
  }

  // gw2 (#203): the bound bits of slots [start, start + count), slot `start` in bit 0. Takes 1 to 64
  // slots, all below NumElements; a range may cross the first 64 slots' word.
  constexpr uint64_t
  bound_range(size_t start, size_t count) const noexcept {
    size_t word = start / 64, shift = start % 64;
    uint64_t bits = bound.qword(word) >> shift;
    if (shift && word + 1 < (NumElements + 63) / 64)
      bits |= bound.qword(word + 1) << (64 - shift);
    return count < 64 ? bits & ((1ull << count) - 1) : bits;
  }

  constexpr uint32_t
  max_binding_64() const noexcept {
    uint64_t qword = dirty.qword(0);
    return 64u - bit::tzcnt(qword);
  }

  inline void
  clear_dirty() {
    dirty.clearAll();
  }

  inline void
  clear_dirty(size_t slot) {
    dirty.set(slot, false);
  }

  inline void
  clear_dirty_mask(uint32_t mask) {
    dirty.clearMask(mask);
  }

  inline void
  set_dirty() {
    dirty.setAll();
  };

  inline void
  set_dirty(size_t slot) {
    dirty.set(slot, true);
  };

  /**
  try to bind element at specific slot, and return a reference to the
  corresponding element storage it also tells if a replacement does happen
  this is useful because we don't have to fully initialize the element
  (so no initialization overhead if no replacement)
  */
  inline Element &
  bind(size_t slot, Element &&element, bool &replacement, bool hazard = true) {
    if (bound.get(slot)) {
      if (redundant_binding_trait<Element>::is_redundant(storage[slot], element)) {
        return storage[slot];
      }
    } else {
      bound.set(slot, true);
      this->hazard.set(slot, hazard);
    }
    // new (storage.data() + slot) Element(std::forward<Element>(element));
    // std::construct_at(storage.data() + slot, std::forward<Element>(element));
    // idk why placement construction kills performance
    storage[slot] = std::forward<Element>(element);
    dirty.set(slot, true);
    replacement = true;
    return storage[slot];
  };

  inline bool
  unbind(size_t slot) {
    if (bound.get(slot)) {
      // storage[slot].~Element();
      // yeah that's weird but since we don't use placement construction
      // (because it's slow-as-f for some weird reason)
      // and Com<T> (typically inside Element) doesn't reset pointer at
      // destruction then a redundant Release occur when trying to move Element
      // at bind()
      storage[slot] = {};
      bound.set(slot, false);
      hazard.set(slot, false);
      dirty.set(slot, true);
      return true;
    }
    return false;
  }

  class bound_iterator {
    const BindingSet &binding_set;
    const bit::bitset<NumElements> &bits;
    size_t current;

    void
    advance_to_next() {
      while (current < NumElements) {
        auto qword_index = current / 64;
        auto tz = bit::tzcnt(uint64_t(bits.qword(qword_index) & ~((1ull << (current % 64)) - 1ull)));
        if (tz < 64) {
          current = qword_index * 64 + tz;
          return;
        }
        current = (qword_index + 1) * 64;
        continue;
      }
      // just in case, clamp it
      current = NumElements;
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<size_t, const Element &>;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = std::pair<size_t, const Element &>;

    bound_iterator(const BindingSet &set, const bit::bitset<NumElements> &bits, size_t start) :
        binding_set(set),
        bits(bits),
        current(start) {
      advance_to_next();
    }

    reference
    operator*() const {
      return std::pair<size_t, const Element&>(current, binding_set.storage[current]);
    }

    bound_iterator &
    operator++() {
      ++current;
      advance_to_next();
      return *this;
    }

    bound_iterator
    operator++(int) {
      bound_iterator temp = *this;
      ++(*this);
      return temp;
    }

    bool
    operator!=(uint64_t index) const {
      return current != index;
    }
  };

  bound_iterator
  begin() const {
    return bound_iterator(*this, bound, 0);
  }
  uint64_t
  end() const {
    return NumElements;
  }

  bound_iterator
  hazard_begin() const {
    return bound_iterator(*this, hazard, 0);
  }
  uint64_t
  hazard_end() const {
    return NumElements;
  }
};

// gw2 (#203): DXMT_SKIP_EMPTY_SLOTS's walk for XXSetShaderResources and XXSetSamplers. Calls
// visit(slot) for the slots [start, start + count) in ascending order. With `skip` on and 1 to 64
// slots in range, it calls it only for a slot whose incoming entry isn't null or that is bound. A null
// entry for a slot that holds nothing changes nothing in the setters: unbind() finds the slot empty,
// and no counter moves. A visit binds or unbinds only its own slot, so the bits read before the
// first visit are still that slot's bits when its turn comes. Wider or out-of-range calls take the
// full walk.
template <typename Element, size_t NumElements, typename Entry, typename Visit>
inline void
visit_settable_slots(
    const BindingSet<Element, NumElements> &set, unsigned start, unsigned count, Entry *const *entries, bool skip,
    Visit &&visit
) {
  if (skip && count - 1u < 64u && uint64_t(start) + count <= NumElements) {
    uint64_t mask = 0;
    for (unsigned i = 0; i < count; i++)
      mask |= uint64_t(entries[i] != nullptr) << i;
    for (mask |= set.bound_range(start, count); mask; mask &= mask - 1)
      visit(start + bit::tzcnt(mask));
    return;
  }
  for (unsigned slot = start; slot < start + count; slot++)
    visit(slot);
}
} // namespace dxmt