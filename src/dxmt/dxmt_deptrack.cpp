#include "dxmt_deptrack.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>

namespace dxmt {

void
GenericAccessTracker::accessShared(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, EncoderId horizon) {
  if (exclusive_ == id) {
    if (isShared)
      return;
    isShared = 1;
    barrier_state.barrierSet |= kBarrierTypeRW;
    return;
  }
  assert(exclusive_ < id);
  if (shared_.isLastAccess(id))
    return;
  shared_.add(id);
  if (id - exclusive_ < horizon) {
    wait_fences.set(exclusive_);
  }
}

void
GenericAccessTracker::accessExclusive(
    EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav, EncoderId horizon
) {
  isShared = 0;
  if (exclusive_ == id) {
    barrier_state.barrierSet |= (1 << uav);
    return;
  }
  if (shared_.isLastAccess(id)) {
    barrier_state.barrierSet |= kBarrierTypeRW;
  }
  shared_.enumerate(id, horizon, [&](EncoderId id) { wait_fences.set(id); });
  shared_.clear();
  if (id - exclusive_ < horizon)
    wait_fences.set(exclusive_);
  exclusive_ = id;
}

void
GenericAccessTracker::accessSharedPreRaster(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, EncoderId horizon) {
  if (exclusive_ == id + 1) {
    if (isSharedPreRaster)
      return;
    isSharedPreRaster = 1;
    if (lastWriteFromPreRaster)
      barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    else
      barrier_state.barrierPreRasterAfterFragmentSet |= kBarrierTypeRW;
    return;
  }
  if (exclusive_ == id) {
    if (isSharedPreRaster)
      return;
    isSharedPreRaster = 1;
    barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    return;
  }
  assert(exclusive_ < id);
  if (shared_.isLastAccess(id + 1)) {
    if (isSharedPreRaster)
      return;
    isSharedPreRaster = 1;
    if (id - exclusive_ < horizon)
      wait_fences.set(exclusive_);
    return;
  } else if (shared_.isLastAccess(id)) {
    // NOP
    return;
  }
  shared_.add(id);
  if (id - exclusive_ < horizon)
    wait_fences.set(exclusive_);
  isSharedPreRaster = 1;
  isShared = 0;
}

void
GenericAccessTracker::accessExclusivePreRaster(
    EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav, EncoderId horizon
) {
  auto last_exclusive = exclusive_;
  exclusive_ = id;

  if (last_exclusive == id + 1) {
    auto last = lastWriteFromPreRaster;
    lastWriteFromPreRaster = 1;
    if (!isShared && !isSharedPreRaster) {
      if (last)
        barrier_state.barrierPreRasterSet |= (1 << uav);
      else
        barrier_state.barrierPreRasterAfterFragmentSet |= (1 << uav);
      return;
    }
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierPreRasterAfterFragmentSet |= kBarrierTypeRW;
    }
    return;
  }
  if (last_exclusive == id) {
    if (!isShared && !isSharedPreRaster) {
      barrier_state.barrierPreRasterSet |= (1 << uav);
    }
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierPreRasterAfterFragmentSet |= kBarrierTypeRW;
    }
    return;
  }
  shared_.enumerate(id, horizon, [&](EncoderId id) { wait_fences.set(id); });
  // gw2 (#132): the compressed policy drops a writer past the horizon, as the other accesses do (its
  // checkpoint chain has finished it); the legacy policy keeps this unconditional wait
  if (last_exclusive && (horizon == kLane || id - last_exclusive < horizon))
    wait_fences.set(last_exclusive);

  if (shared_.isLastAccess(id + 1) || shared_.isLastAccess(id)) {
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierPreRasterAfterFragmentSet |= kBarrierTypeRW;
    }
    shared_.clear();
    return;
  }
  shared_.clear();
  isShared = 0;
  isSharedPreRaster = 0;
}

void
GenericAccessTracker::accessSharedFragment(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, EncoderId horizon) {
  if (exclusive_ == id) {
    if (isShared)
      return;
    isShared = 1;
    if (isSharedPreRaster)
      return; // IMPLICIT BARRIER
    if (lastWriteFromPreRaster)
      barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    else
      barrier_state.barrierSet |= kBarrierTypeRW;
    return;
  }
  if (exclusive_ == id - 1) {
    if (isShared)
      return;
    isShared = 1;
    // gw2 (#132): the pass wrote this at pre-raster and now reads it at fragment. The legacy policy
    // remembers only the write (exclusive_ = id - 1), so a later writer waits for this pass's
    // pre-raster stage, not for this read; its weak window hides that from 64 encoders back. The
    // compressed policy records the read, so the later writer waits for the fragment stage.
    if (horizon != kLane)
      shared_.add(id);
    if (isSharedPreRaster)
      return; // IMPLICIT BARRIER
    barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    return;
  }
  assert(exclusive_ < id - 1);
  if (shared_.isLastAccess(id))
    return;
  bool isVertexLastAccess = shared_.isLastAccess(id - 1);
  if (id - exclusive_ < horizon)
    wait_fences.set(exclusive_);
  shared_.add(id);
  isShared = 1;
  if (!isVertexLastAccess)
    isSharedPreRaster = 0;
}

void
GenericAccessTracker::accessExclusiveFragment(
    EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav, EncoderId horizon
) {
  auto last_exclusive = exclusive_;
  exclusive_ = id;
  if (last_exclusive == id) {
    auto last = lastWriteFromPreRaster;
    lastWriteFromPreRaster = 0;
    if (!isShared && !isSharedPreRaster) {
      if (last)
        barrier_state.barrierFragmentAfterPreRasterSet |= (1 << uav);
      else
        barrier_state.barrierSet |= (1 << uav);
      return;
    }
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierSet |= kBarrierTypeRW;
    }
    return;
  }
  lastWriteFromPreRaster = 0;
  if (last_exclusive == id - 1) {
    if (!isShared && !isSharedPreRaster) {
      barrier_state.barrierFragmentAfterPreRasterSet |= (1 << uav);
      return;
    }
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierSet |= kBarrierTypeRW;
    }
    return;
  }
  shared_.enumerate(id, horizon, [&](EncoderId id) { wait_fences.set(id); });
  // gw2 (#132): the compressed policy drops a writer past the horizon, as the other accesses do (its
  // checkpoint chain has finished it); the legacy policy keeps this unconditional wait
  if (last_exclusive && (horizon == kLane || id - last_exclusive < horizon))
    wait_fences.set(last_exclusive);
  if (shared_.isLastAccess(id) || shared_.isLastAccess(id - 1)) {
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierSet |= kBarrierTypeRW;
    }
    shared_.clear();
    return;
  }
  shared_.clear();
  isShared = 0;
  isSharedPreRaster = 0;
}

class WeakFenceMaskLTO {
public:
  constexpr WeakFenceMaskLTO() {
    int i = 0;
    for (int p = 0; p < kParity; ++p) {
      for (int l = 0; l < kLane; ++l) {
        weak_fences_lto[i++].fillGenerationBefore(p, l);
      }
    }
  }

  const FenceSet &
  operator[](EncoderId i) const {
    return weak_fences_lto[i % kParityLane];
  }

private:
  FenceSet weak_fences_lto[kParityLane];
};

constexpr auto WEAK_FENCE_MASK = WeakFenceMaskLTO();

FenceSet
FenceLocalityCheck::collectLegacy(FenceSet strong_fences, EncoderId id, bool implicit_pre_raster_wait) {
  if (implicit_pre_raster_wait)
    strong_fences.set(id - 1);

  FenceSet full_fences(strong_fences);
  full_fences.mergeWithLaneMaskOff(WEAK_FENCE_MASK[id], strong_fences.laneMask());

  FenceSet minimal_fences;
  FenceSet accessible_fences;

  constexpr auto start_offset = kParityLane == 1 ? 0 : 1;

  for (auto offset = start_offset; offset < kParityLane; offset++) {
    EncoderId prev_encoder_id = id - offset;

    if (full_fences.test(prev_encoder_id) && !accessible_fences.testAndSet(prev_encoder_id))
      minimal_fences.set(prev_encoder_id);
    if (accessible_fences.test(prev_encoder_id))
      accessible_fences.merge(summary_[prev_encoder_id % kParityLane]);
    if (accessible_fences.contains(full_fences))
      break;
  }

  summary_[id % kParityLane] = full_fences;

  if (implicit_pre_raster_wait)
    minimal_fences.unset(id - 1);

  return minimal_fences;
}

DependencyCounters &
dependencyCounters() {
  static DependencyCounters counters;
  return counters;
}

FenceSet
FenceLocalityCheck::collectAndSimplifyWaits(FenceSet strong_fences, EncoderId id, bool implicit_pre_raster_wait) {
  if (!counting_)
    return compressed_ ? collectCompressed(strong_fences, id, implicit_pre_raster_wait)
                       : collectLegacy(strong_fences, id, implicit_pre_raster_wait);
  // gw2 (#132): DEPS counters, one clock pair per encoder stage (about 60 a frame)
  auto &counters = dependencyCounters();
  auto t0 = std::chrono::steady_clock::now();
  auto strong = strong_fences.count();
  auto waits = compressed_ ? collectCompressed(strong_fences, id, implicit_pre_raster_wait)
                           : collectLegacy(strong_fences, id, implicit_pre_raster_wait);
  auto t1 = std::chrono::steady_clock::now();
  counters.encoders.fetch_add(1, std::memory_order_relaxed);
  counters.strong.fetch_add(strong, std::memory_order_relaxed);
  counters.full.fetch_add(summary_[id % kParityLane].count(), std::memory_order_relaxed);
  counters.simplified.fetch_add(waits.count(), std::memory_order_relaxed);
  counters.ns.fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(), std::memory_order_relaxed
  );
  return waits;
}

// gw2 (#132, after GameSir 3170d0349a, which removes the weak window; this keeps a guarantee in its
// place): the checkpoint chain. Our trackers stay bounded (they drop dependencies past a horizon, and
// keep a bounded ring of readers), so without the window something must still cover what they drop.
// (GameSir instead tracks every reader and writer as an explicit ID, relying on a Metal fence wait
// covering every earlier update of its slot.)
//
// The legacy policy adds a weak window (the encoders 64-127 IDs back) to every encoder, so by
// induction everything up to C-64 has finished when C runs, and a tracker may drop older
// dependencies. After simplification that is about one extra wait per encoder. Here only a
// checkpoint, about every 64 IDs, carries a window: it waits on every encoder from the previous
// checkpoint's coverage up to C-64 (all of them; simplification removes the reachable ones) and on
// the previous checkpoint. Every other encoder adds the latest checkpoint S to its dependencies,
// which simplification usually removes (the previous encoder already depended on S).
//
// Invariant: when C runs, every encoder up to C-127 has finished.
// - Checkpoint S, previous S' (S - S' is 63 or 64 with contiguous IDs): S waits on (covered(S'), S-64]
//   and on S'; S' finished means everything up to covered(S') finished. So covered(S) = S-64.
// - Any other C: S >= C-63 is in C's dependencies, so everything up to S-64 >= C-127 has finished.
// - The trackers keep direct dependencies up to kCompressedHorizon-1 = 127 back, so what they drop
//   (C-128 and older) has finished.
// - Waits never name an encoder more than kParityLane-1 back, so the wanted encoder's update is the
//   newest of its slot (the slot is reused only kParityLane IDs later); the proof doesn't need Metal's
//   wait covering a slot's older updates too, though Metal 4 documents that it does.
// - IDs out of order (counted as gaps; the context hands them out contiguously) only make the
//   window wider, clamped to kParityLane-1 back: a wait on every slot of that range waits on the
//   newest encoders, which carry the invariant for everything older.
FenceSet
FenceLocalityCheck::collectCompressed(FenceSet strong_fences, EncoderId id, bool implicit_pre_raster_wait) {
  assert(id >= kParityLane);
  if (last_ && id != last_ + 1)
    dependencyCounters().gaps.fetch_add(1, std::memory_order_relaxed);
  last_ = id;
  if (!first_)
    first_ = id;

  if (implicit_pre_raster_wait)
    strong_fences.set(id - 1);

  FenceSet full_fences(strong_fences);
  last_checkpoint_ = !checkpoint_ || id - checkpoint_ >= kCheckpointStride;
  if (last_checkpoint_) {
    EncoderId lo = std::max({covered_ + 1, id - (kParityLane - 1), first_});
    EncoderId hi = id - kLane;
    for (EncoderId x = lo; x <= hi; x++)
      full_fences.set(x);
    if (checkpoint_ && id - checkpoint_ < kParityLane)
      full_fences.set(checkpoint_);
    covered_ = hi;
    checkpoint_ = id;
    if (counting_)
      dependencyCounters().checkpoints.fetch_add(1, std::memory_order_relaxed);
  } else {
    full_fences.set(checkpoint_);
  }

  FenceSet minimal_fences;
  FenceSet accessible_fences;

  for (EncoderId offset = 1; offset < kParityLane; offset++) {
    EncoderId prev_encoder_id = id - offset;
    if (prev_encoder_id < first_)
      break;

    if (full_fences.test(prev_encoder_id) && !accessible_fences.testAndSet(prev_encoder_id))
      minimal_fences.set(prev_encoder_id);
    // a summary counts only for the encoder that wrote it (GameSir's generation check)
    if (accessible_fences.test(prev_encoder_id) &&
        summary_generation_[prev_encoder_id % kParityLane] == prev_encoder_id)
      accessible_fences.merge(summary_[prev_encoder_id % kParityLane]);
    if (accessible_fences.contains(full_fences))
      break;
  }

  summary_[id % kParityLane] = full_fences;
  summary_generation_[id % kParityLane] = id;

  if (implicit_pre_raster_wait)
    minimal_fences.unset(id - 1);

  return minimal_fences;
}

} // namespace dxmt
