#pragma once

// gw2 (#66, L1): the retirement ledger (architecture council 2026-11-04 §3; ADR 0003 in
// docs/gw2/adr/). What a chunk's command buffers use and nothing else holds is held here until the
// finish thread retires the chunk: the present split's command buffers, Metal objects (the drawable,
// events), the presenter and the MetalFX scalers.
//
// - gw2 (#69, L2): which of them the chunk must hold is the ledger's retention policy, not the call
//   site's: every hold names its reason, and a hold the policy declines leaves the object with the
//   caller (released where it always was).
// - Holds are typed and stored per ring slot (seq % ring slots), stamped with the chunk sequence that
//   owns the slot, so a hold for a slot still owned by an unretired chunk (a stale generation) is
//   refused rather than mixed into it. A ring slot's lists keep their capacity across retirements, as the
//   chunk's lists did.
// - Retirements come one at a time in sequence order (the finish thread's), each releasing only its
//   own chunk's holds, in the fixed order below. A repeated or out-of-order retirement is refused.
// - A refused hold is never released early: it is kept until drain(). The caller logs the refusal;
//   it is a bug, never expected.
//
// Allocation tracking, the ring allocators and the handshake counters are not here; the queue's
// RetireChunk calls them in their existing order around retire().
//
// Generic over its owned types (Traits) so the native test can count releases; production's traits
// are RetirementTraits in dxmt_command_queue.hpp. Threads: hold on the encode thread, retire on the
// finish thread, drain after both stopped; the queue's ready_for_commit / cpu_coherent handshake
// orders a slot's use between them, as it ordered the chunk's lists.

#include <atomic>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace dxmt::gw2 {

// gw2 (#69): why a chunk holds something
enum class HoldReason : uint8_t {
  PresentSplit,   // a command buffer the present split committed early (the finish thread reads it)
  Drawable,       // the present's drawable (its texture is the present pass's attachment)
  Presenter,      // the present pass's pipelines and gamma table
  Event,          // a Signal/Wait event (an ID3D11Fence, a keyed mutex) that may go first
  SpatialScaler,  // a MetalFX spatial scaler and its fence
  TemporalScaler, // a MetalFX temporal scaler and its fence
};

// gw2 (#69): which holds a queue's chunks need. Only Metal 3's retained command buffers
// ([queue commandBuffer], DXMT_UNRETAINED=0) hold what they use themselves, and then only when the
// lifetime check (whose stamps assume DXMT holds) is off; Metal 4's retain nothing. The present
// split's buffers are always held: the finish thread checks them for errors and reads their GPU spans.
struct RetentionPolicy {
  bool metal4 = false;         // the queue's chunks go to Metal 4 (which runs only with DXMT_UNRETAINED=1)
  bool unretained = false;     // DXMT_UNRETAINED=1: Metal 3 command buffers retain nothing
  bool lifetime_check = false; // DXMT_LIFETIME_CHECK=1 (a diagnostic build)

  bool
  commandBuffersRetain() const {
    return !metal4 && !unretained && !lifetime_check;
  }
  bool
  holds(HoldReason reason) const {
    return reason == HoldReason::PresentSplit || !commandBuffersRetain();
  }
};

// Traits: Object (an owned raw handle; null is false; Traits::release(Object) drops it),
// CommandBuffer, Presenter, SpatialScaler, TemporalScaler (owning types whose destructor releases).
template <class Traits> class BasicRetirementLedger {
public:
  using Object = typename Traits::Object;
  using CommandBuffer = typename Traits::CommandBuffer;
  using Presenter = typename Traits::Presenter;
  using SpatialScaler = typename Traits::SpatialScaler;
  using TemporalScaler = typename Traits::TemporalScaler;

  BasicRetirementLedger(uint32_t ring_slots, RetentionPolicy policy) :
      slots_(ring_slots),
      ring_slots_(ring_slots),
      policy_(policy) {}

  BasicRetirementLedger(const BasicRetirementLedger &) = delete;
  BasicRetirementLedger &operator=(const BasicRetirementLedger &) = delete;

  ~BasicRetirementLedger() {
    for (auto &slot : slots_)
      release(slot);
    releaseRefused();
  }

  // gw2 (#69): the queue's policy, set before its first chunk (once it has picked Metal 3 or 4)
  void
  setPolicy(RetentionPolicy policy) {
    policy_ = policy;
  }
  const RetentionPolicy &
  policy() const {
    return policy_;
  }

  // Encode thread, chunk `seq` being encoded. A null object, or one the policy doesn't need held,
  // holds nothing and stays with the caller (true). False: refused (a retired sequence, or the ring
  // slot belongs to an unretired one); the object is kept until drain().
  [[nodiscard]] bool
  hold(uint64_t seq, Object &&object, HoldReason reason) {
    if (!object || !policy_.holds(reason))
      return true;
    auto *slot = ringSlotFor(seq);
    (slot ? *slot : refused_).objects.push_back(std::exchange(object, Object{}));
    return slot != nullptr;
  }
  [[nodiscard]] bool
  hold(uint64_t seq, Presenter &&presenter) {
    return holdIn(seq, std::move(presenter), HoldReason::Presenter, &RingSlot::presenters);
  }
  [[nodiscard]] bool
  hold(uint64_t seq, SpatialScaler &&scaler) {
    return holdIn(seq, std::move(scaler), HoldReason::SpatialScaler, &RingSlot::spatial_scalers);
  }
  [[nodiscard]] bool
  hold(uint64_t seq, TemporalScaler &&scaler) {
    return holdIn(seq, std::move(scaler), HoldReason::TemporalScaler, &RingSlot::temporal_scalers);
  }

  // Encode thread: a command buffer split off chunk `seq` (the present split), committed before the
  // chunk's last; held for the finish thread's error check and GPU spans (HoldReason::PresentSplit:
  // always), then released first.
  template <class B>
  [[nodiscard]] bool
  holdSplit(uint64_t seq, B &&cmdbuf) {
    auto *slot = ringSlotFor(seq);
    (slot ? *slot : refused_).splits.emplace_back(std::forward<B>(cmdbuf));
    return slot != nullptr;
  }

  // Finish thread, before retire(seq): chunk `seq`'s split-off command buffers, in commit order.
  std::span<CommandBuffer>
  splits(uint64_t seq) {
    auto &slot = slots_[seq % ring_slots_];
    if (slot.seq != seq)
      return {};
    return slot.splits;
  }

  // Finish thread: chunk `seq` completed and every completion consumer is done. Releases its holds:
  // split command buffers, objects, presenters, spatial scalers, temporal scalers. False: refused
  // (not the next sequence: repeated, skipped, or after drain()); nothing is released.
  [[nodiscard]] bool
  retire(uint64_t seq) {
    uint64_t retired = retired_.load(std::memory_order_relaxed);
    if (seq != retired + 1 || retired == kDrained)
      return false;
    // from here a hold for `seq` is refused (even one made by a release below)
    retired_.store(seq, std::memory_order_release);
    auto &slot = slots_[seq % ring_slots_];
    if (slot.seq == seq)
      release(slot);
    return true;
  }

  // Teardown, after the encode and finish threads stopped: releases every ring slot's holds in ring-slot
  // order, calling before_slot(ring slot index) first (the queue resets the chunk of the same index there,
  // which keeps today's per-chunk order), then the refused holds. Holds and retirements after it are
  // refused.
  template <class F>
  void
  drain(F &&before_slot) {
    retired_.store(kDrained, std::memory_order_release);
    for (uint32_t i = 0; i < ring_slots_; i++) {
      before_slot(i);
      release(slots_[i]);
    }
    releaseRefused();
  }
  void
  drain() {
    drain([](uint32_t) {});
  }

  // the last sequence retired (0: none yet)
  uint64_t
  retired() const {
    return retired_.load(std::memory_order_acquire);
  }

  uint32_t
  ringSlots() const {
    return ring_slots_;
  }

private:
  static constexpr uint64_t kDrained = ~0ull;

  struct RingSlot {
    uint64_t seq = 0; // the chunk whose holds these are; free once it is <= retired_
    std::vector<CommandBuffer> splits;
    std::vector<Object> objects;
    std::vector<Presenter> presenters;
    std::vector<SpatialScaler> spatial_scalers;
    std::vector<TemporalScaler> temporal_scalers;
  };

  // the ring slot chunk `seq` may hold in, or null if the hold is refused
  RingSlot *
  ringSlotFor(uint64_t seq) {
    uint64_t retired = retired_.load(std::memory_order_acquire);
    if (seq <= retired)
      return nullptr; // retired (or retiring), or drained
    auto &slot = slots_[seq % ring_slots_];
    if (slot.seq != seq) {
      if (slot.seq > retired)
        return nullptr; // a stale generation: another unretired chunk owns the ring slot
      slot.seq = seq;   // free: its previous chunk retired and released everything
    }
    return &slot;
  }

  template <class T>
  bool
  holdIn(uint64_t seq, T &&owner, HoldReason reason, std::vector<T> RingSlot::*list) {
    if (!owner || !policy_.holds(reason))
      return true;
    auto *slot = ringSlotFor(seq);
    ((slot ? *slot : refused_).*list).push_back(std::move(owner));
    return slot != nullptr;
  }

  // the existing cross-category order (CommandChunk::reset() before #66): the split command buffers,
  // then the objects, the presenters, the spatial and the temporal scalers
  static void
  release(RingSlot &slot) {
    slot.splits.clear();
    for (auto &object : slot.objects)
      Traits::release(object);
    slot.objects.clear();
    slot.presenters.clear();
    slot.spatial_scalers.clear();
    slot.temporal_scalers.clear();
  }

  // moved out first: a hold made by one of these releases is refused into a fresh refused_
  void
  releaseRefused() {
    RingSlot refused = std::move(refused_);
    refused_ = RingSlot{};
    release(refused);
  }

  std::vector<RingSlot> slots_;
  RingSlot refused_; // refused holds, kept until drain()
  const uint32_t ring_slots_;
  RetentionPolicy policy_;
  std::atomic<uint64_t> retired_{0};
};

} // namespace dxmt::gw2
