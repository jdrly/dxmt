#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include "util_bit.hpp"

namespace dxmt {

namespace ResourceAccess {
constexpr int Read = 1 << 0;
constexpr int Write = 1 << 1;
constexpr int ReadWrite = Read | Write;
constexpr int UAV = 1 << 2;
constexpr int All = ReadWrite | UAV;
}; // namespace ResourceAccess

constexpr auto kLog2Lane = 6ull;
constexpr auto kLane = 1 << kLog2Lane;
constexpr auto kLaneMask = kLane - 1;
constexpr auto kAllLaneMask = ~0ull >> (64 /* uint64_t */ - kLane);
constexpr auto kParity = 4; // can also use 3, although power of 2 is nice
constexpr auto kParityLane = kParity * kLane;

// gw2 (#132): with DXMT_DEPENDENCY_COMPRESSION the trackers keep direct dependencies this many
// encoder IDs back (the legacy policy keeps kLane); FenceLocalityCheck's checkpoint chain covers
// everything older (see collectAndSimplifyWaits).
constexpr auto kCompressedHorizon = 2 * kLane;
constexpr auto kCheckpointStride = kLane - 1;

static_assert(kLog2Lane <= 6);
static_assert(kCompressedHorizon < kParityLane);
static_assert(kLane > 1);

using LaneStorage = uint64_t;
using EncoderId = uint64_t;

constexpr auto
PARITY(EncoderId id) {
  return (id >> kLog2Lane) % kParity;
}

constexpr auto
LANE(EncoderId id) {
  return id & kLaneMask;
}

class FenceSet {
public:
  constexpr FenceSet() {
    for (int i = 0; i < kParity; i++) {
      storage_[i] = 0;
    }
  }

  constexpr FenceSet(EncoderId id) {
    for (int i = 0; i < kParity; i++) {
      storage_[i] = 0;
    }
    set(id);
  }

  FenceSet(const FenceSet &copy) {
    memcpy(&storage_, &copy.storage_, sizeof(storage_));
  }

  FenceSet &
  operator=(const dxmt::FenceSet &copy) {
    memcpy(&storage_, &copy.storage_, sizeof(storage_));
    return *this;
  }

  ~FenceSet() = default;

  constexpr void
  set(EncoderId id) {
    storage_[PARITY(id)] |= (1ull << LANE(id));
  }

  constexpr void
  unset(EncoderId id) {
    storage_[PARITY(id)] &= (kAllLaneMask & ~(1ull << LANE(id)));
  }

  constexpr void
  fillGenerationBefore(int parity, int lane) {
    const int idx = (parity + kParity + (kParity - 1)) * kLane + lane;
    for (int offset = 0; offset < kLane; ++offset) {
      set(idx - offset);
    }
  }

  constexpr bool
  test(EncoderId id) const {
    return storage_[PARITY(id)] & (1ull << LANE(id));
  }

  constexpr bool
  testAndSet(EncoderId id) {
    auto P = PARITY(id);
    auto LM = 1ull << LANE(id);
    if (storage_[P] & LM)
      return true;
    storage_[P] |= LM;
    return false;
  }

  constexpr bool
  intersectedWith(const FenceSet &set) const {
    for (int i = 0; i < kParity; i++) {
      if (storage_[i] & set.storage_[i])
        return true;
    }
    return false;
  }

  constexpr bool
  contains(const FenceSet &set) const {
    for (int i = 0; i < kParity; i++) {
      if ((storage_[i] & set.storage_[i]) != set.storage_[i])
        return false;
    }
    return true;
  }

  FenceSet &
  merge(const FenceSet &set) {
    for (int i = 0; i < kParity; i++) {
      storage_[i] |= set.storage_[i];
    }
    return *this;
  }

  FenceSet
  unionOf(const FenceSet &set) const {
    FenceSet ret{};
    for (int i = 0; i < kParity; i++) {
      ret.storage_[i] = storage_[i] | set.storage_[i];
    }
    return ret;
  }

  FenceSet &
  subtract(const FenceSet &set) {
    for (int i = 0; i < kParity; i++) {
      storage_[i] &= (kAllLaneMask & ~set.storage_[i]);
    }
    return *this;
  }

  FenceSet &
  mergeWithLaneMaskOff(const FenceSet &set, const LaneStorage &mask) {
    for (int i = 0; i < kParity; i++) {
      storage_[i] |= (set.storage_[i] & (kAllLaneMask & ~mask));
    }
    return *this;
  }

  LaneStorage
  laneMask() const {
    LaneStorage ret = 0;
    for (int i = 0; i < kParity; i++) {
      ret |= storage_[i];
    }
    return ret;
  }

  bool
  empty() const {
    return laneMask() == 0;
  }

  // gw2 (#132): how many fences the set names
  uint32_t
  count() const {
    uint32_t ret = 0;
    for (int i = 0; i < kParity; i++)
      ret += __builtin_popcountll(storage_[i]);
    return ret;
  }

  template <typename Fn>
  void
  forEach(Fn &&fn) {
    for (int P = 0; P < kParity; P++) {
      auto lanes = storage_[P];
      while (lanes) {
        auto lane = bit::tzcnt(lanes);
        fn(P * kLane + lane);
        lanes &= ~(1ull << lane);
      }
    }
  }

  template <typename Fn, typename FnPrior>
  void
  forEach(const FenceSet &prior, FnPrior &&fnPrior, Fn &&fn) {
    for (int P = 0; P < kParity; P++) {
      auto lanes = storage_[P];
      auto lanes_prior = prior.storage_[P];
      while (auto lanes_combine = lanes | lanes_prior) {
        auto lane = bit::tzcnt(lanes_combine);
        if (lanes_prior & (1ull << lane))
          fnPrior(P * kLane + lane);
        else
          fn(P * kLane + lane);
        lanes &= ~(1ull << lane);
        lanes_prior &= ~(1ull << lane);
      }
    }
  }

private:
  LaneStorage storage_[kParity];
};

template <size_t Sz = kLane, size_t Forward = 1> class TrackingSet {
public:
  TrackingSet() {
    cursor = 0;
    clear();
  };

  bool
  add(EncoderId id) {
    assert(storage_[cursor] <= id);
    if (storage_[cursor] == id)
      return false;
    {
      cursor++;
      cursor = cursor % Sz;
    }
    storage_[cursor] = id;
    return true;
  };

  bool
  isLastAccess(EncoderId id) {
    return storage_[cursor] == id;
  }

  void
  clear() {
    storage_[cursor] = 0;
  };

  // gw2 (#132): `horizon` (<= Sz) is how far back an access still counts; the legacy policy passes kLane
  template <typename Fn>
  size_t
  enumerate(EncoderId id_before, EncoderId horizon, Fn &&fn) {
    size_t count = 0;
    assert(id_before > Sz && horizon <= Sz);
    for (size_t i = 0; i < Sz; i++) {
      auto c = storage_[(cursor + Sz - i) % Sz];
      if (c >= id_before) {
        assert(c - id_before <= Forward);
        continue;
      }
      if (c > (id_before - horizon)) {
        fn(c);
        count++;
        continue;
      }
      break;
    }
    return count;
  }

private:
  EncoderId storage_[Sz + Forward];
  uint32_t cursor;
};

constexpr auto kBarrierTypeRW = 1 << 0;
constexpr auto kBarrierTypeWaW = 1 << 1;

struct EncoderBarrierState {
  uint64_t barrierSet                       : 2 = 0;
  uint64_t barrierPreRasterSet              : 2 = 0;
  uint64_t barrierFragmentAfterPreRasterSet : 2 = 0;
  uint64_t barrierPreRasterAfterFragmentSet : 2 = 0;
  uint64_t reserved                         : 56;
};

// gw2 (#132): every access takes the context's horizon: kLane (legacy) or kCompressedHorizon
class GenericAccessTracker {
public:
  void accessShared(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, EncoderId horizon = kLane);
  void accessExclusive(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav, EncoderId horizon = kLane);

  void accessSharedPreRaster(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, EncoderId horizon = kLane);
  void accessExclusivePreRaster(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav, EncoderId horizon = kLane);
  void accessSharedFragment(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, EncoderId horizon = kLane);
  void accessExclusiveFragment(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav, EncoderId horizon = kLane);

private:
  /**
   * Previous shared access (gw2 #132: room for kCompressedHorizon readers)
   */
  TrackingSet<kCompressedHorizon> shared_;
  /**
   * Last exclusive access
   */
  EncoderId exclusive_{};
  uint64_t isShared               : 1 = 0;
  uint64_t isSharedPreRaster      : 1 = 0;
  uint64_t lastWriteFromPreRaster : 1 = 0;
};

// gw2 (#132): DEPS section counters (the section is dxmt_gw2_deps.cpp), added by every context's
// FenceLocalityCheck and taken by the 2 s statistics window.
struct DependencyCounters {
  std::atomic<uint64_t> encoders{};    // collectAndSimplifyWaits calls (one per fenced encoder stage)
  std::atomic<uint64_t> strong{};      // dependencies the trackers reported
  std::atomic<uint64_t> full{};        // after the weak window or the checkpoint were added
  std::atomic<uint64_t> waits{};       // waits left after simplification (what the encoders emit)
  std::atomic<uint64_t> checkpoints{}; // compressed: checkpoint encoders
  std::atomic<uint64_t> gaps{};        // compressed: an ID came out of order (never expected)
  std::atomic<uint64_t> ns{};          // time in collectAndSimplifyWaits (when timing is on)
};
DependencyCounters &dependencyCounters();

class FenceLocalityCheck {
public:
  FenceSet collectAndSimplifyWaits(FenceSet strong_fences, EncoderId id, bool implicit_pre_raster_wait = false);

  // gw2 (#132): choose the policy before the first encoder; it never changes afterwards
  void setCompressed(bool compressed) { compressed_ = compressed; }
  bool compressed() const { return compressed_; }
  EncoderId horizon() const { return compressed_ ? kCompressedHorizon : kLane; }
  void setCounting(bool counting) { counting_ = counting; }

private:
  FenceSet collectLegacy(FenceSet strong_fences, EncoderId id, bool implicit_pre_raster_wait);
  FenceSet collectCompressed(FenceSet strong_fences, EncoderId id, bool implicit_pre_raster_wait);

  std::array<FenceSet, kParityLane> summary_;
  // gw2 (#132): the compressed policy's state
  std::array<EncoderId, kParityLane> summary_generation_ = {};
  bool compressed_ = false;
  bool counting_ = false;
  EncoderId checkpoint_ = 0; // the latest checkpoint encoder
  EncoderId covered_ = 0;    // when checkpoint_ has finished, so has every encoder up to this ID
  EncoderId first_ = 0;      // the first ID this context saw
  EncoderId last_ = 0;       // the latest ID this context saw
};

} // namespace dxmt
