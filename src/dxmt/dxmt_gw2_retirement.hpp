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
// - gw2 (#91, L3): only GPU completion authorizes a release (ADR 0003). retire() takes the completed
//   chunk token from the submission's wait(seq), drain() the drained-queue token from its drain()
//   (dxmt_gw2_completion.hpp); a token from any issuer but the queue's submission (acceptTokensFrom),
//   or an empty one, is refused like a repeated retirement.
// - A refused hold is never released early: it is kept until drain(). The caller logs the refusal;
//   it is a bug, never expected.
// - gw2 (#113): unless the queue recovers (retireOrRecover, DXMT_RETIRE_RECOVERY): a refused retirement
//   then retires by sequence with the completion its token proves, and a refused hold goes once the
//   last chunk that may use it completed. retire() and drain() themselves are unchanged.
//
// Allocation tracking, the ring allocators and the handshake counters are not here; the queue's
// RetireChunk calls them in their existing order around retire().
//
// Generic over its owned types (Traits) so the native test can count releases; production's traits
// are RetirementTraits in dxmt_command_queue.hpp. Threads: hold on the encode thread, retire on the
// finish thread, drain after both stopped; the queue's ready_for_commit / cpu_coherent handshake
// orders a slot's use between them, as it ordered the chunk's lists.

#include "dxmt_gw2_completion.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
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

  // gw2 (#91): the queue's chunk submission, the one issuer whose tokens retire and drain this ledger;
  // set once, before the queue's first chunk (until then every token is refused)
  void
  acceptTokensFrom(const CompletionIssuer *issuer) {
    issuer_ = issuer;
  }

  // Encode thread, chunk `seq` being encoded. A null object, or one the policy doesn't need held,
  // holds nothing and stays with the caller (true). False: refused (a retired sequence, or the ring
  // slot belongs to an unretired one); the object is kept until drain().
  [[nodiscard]] bool
  hold(uint64_t seq, Object &&object, HoldReason reason) {
    if (!object || !policy_.holds(reason))
      return true;
    if (auto *slot = ringSlotFor(seq)) {
      slot->objects.push_back(std::exchange(object, Object{}));
      return true;
    }
    refuse(seq, [&](RingSlot &refused) { refused.objects.push_back(std::exchange(object, Object{})); });
    return false;
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
    if (auto *slot = ringSlotFor(seq)) {
      slot->splits.emplace_back(std::forward<B>(cmdbuf));
      return true;
    }
    refuse(seq, [&](RingSlot &refused) { refused.splits.emplace_back(std::forward<B>(cmdbuf)); });
    return false;
  }

  // Finish thread, before retire(seq): chunk `seq`'s split-off command buffers, in commit order.
  std::span<CommandBuffer>
  splits(uint64_t seq) {
    auto &slot = slots_[seq % ring_slots_];
    if (slot.seq != seq)
      return {};
    return slot.splits;
  }

  // Finish thread: `chunk` (the token of the submission's wait for it) completed and every completion
  // consumer is done. Releases its holds: split command buffers, objects, presenters, spatial scalers,
  // temporal scalers. False: refused (gw2 #91: an empty token or another issuer's; not the next
  // sequence: repeated, skipped, or after drain()); nothing is released.
  template <class Record>
  [[nodiscard]] bool
  retire(const BasicCompletedChunk<Record> &chunk) {
    if (!chunk || chunk.issuer() != issuer_)
      return false;
    uint64_t seq = chunk.seq();
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

  // gw2 (#113): what retireOrRecover() did
  struct Outcome {
    bool retired = false;     // retire() took the token: the normal path
    bool recovered = false;   // retire() refused it, and the ledger recovered with the completion it proves
    bool follows = false;     // the previous call was refused too: the same failure (logged with that one)
    uint64_t before = 0;      // the last sequence retired before the call
    uint64_t after = 0;       // ... and after it
    uint32_t chunks = 0;      // recovery: the chunks (with holds) retired by sequence, before + 1 .. after
    size_t refused_holds = 0; // refused holds released because the last chunk that may use them completed
  };

  // gw2 (#113): finish thread, the queue's retirement with DXMT_RETIRE_RECOVERY (on). retire(chunk) first;
  // nothing else happens on the normal path unless refused holds wait (below).
  // - Refused, with a token of this ledger's issuer (before drain()): it proves chunk `seq` completed, and
  //   since the finish thread waits for every chunk in sequence order (ChunkSubmission's contract), every
  //   earlier one too. So the ledger retires by sequence: the chunks after the last one retired up to
  //   `seq` release their holds in sequence order (their ring slots are free again, so later holds no
  //   longer pile up as refused), and the ring resumes at `seq` + 1. A token at or behind the last
  //   sequence retired releases no ring slot (the slots were released) and moves nothing back.
  // - Refused with an empty token or another issuer's: no completion is known; nothing is released (the
  //   next chunk's token recovers).
  // - Either way (retired or recovered, never before drain() or after it): the refused holds whose last
  //   chunk is at or before `seq` are released (refuse() below). Never early: only completion releases.
  // A refusal right after a refused call `follows` it (the next token catching up after an empty one, a
  // stall's later chunks): the queue logs one error line per failure, not per chunk.
  template <class Record>
  [[nodiscard]] Outcome
  retireOrRecover(const BasicCompletedChunk<Record> &chunk) {
    Outcome out;
    out.before = retired_.load(std::memory_order_relaxed);
    out.retired = retire(chunk);
    out.follows = !out.retired && after_refusal_;
    after_refusal_ = !out.retired;
    if (!out.retired) {
      if (!chunk || chunk.issuer() != issuer_ || out.before == kDrained) {
        out.after = out.before;
        return out;
      }
      out.recovered = true;
      if (chunk.seq() > out.before)
        out.chunks = retireThrough(out.before, chunk.seq());
    }
    out.after = retired();
    if (refused_holds_.load(std::memory_order_relaxed))
      out.refused_holds = releaseRefusedThrough(chunk.seq());
    return out;
  }

  // Teardown, after the encode and finish threads stopped, with `drained` (gw2 #91: the token of the
  // submission's drain()): releases every ring slot's holds in ring-slot order, calling
  // before_slot(ring slot index) first (the queue resets the chunk of the same index there, which keeps
  // today's per-chunk order), then the refused holds. Holds and retirements after it are refused.
  // False: refused (an empty token or another issuer's); nothing is released or called, and the holds
  // stay until the ledger is destroyed.
  template <class F>
  [[nodiscard]] bool
  drain(const DrainedQueue &drained, F &&before_slot) {
    if (!drained || drained.issuer() != issuer_)
      return false;
    retired_.store(kDrained, std::memory_order_release);
    for (uint32_t i = 0; i < ring_slots_; i++) {
      before_slot(i);
      release(slots_[i]);
    }
    releaseRefused();
    return true;
  }
  [[nodiscard]] bool
  drain(const DrainedQueue &drained) {
    return drain(drained, [](uint32_t) {});
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

  // gw2 (#113): the refused holds not released yet (any thread; exact once the threads stopped)
  size_t
  refusedHolds() const {
    return refused_holds_.load(std::memory_order_relaxed);
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
    if (auto *slot = ringSlotFor(seq)) {
      (slot->*list).push_back(std::move(owner));
      return true;
    }
    refuse(seq, [&](RingSlot &refused) { (refused.*list).push_back(std::move(owner)); });
    return false;
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

  static size_t
  count(const RingSlot &slot) {
    return slot.splits.size() + slot.objects.size() + slot.presenters.size() + slot.spatial_scalers.size() +
           slot.temporal_scalers.size();
  }

  // gw2 (#113): a refused hold for chunk `seq`, added by `add` to the refused holds of the last chunk that
  // may use it: `seq` itself while unretired (a stale generation: the chunk is in flight); a retired
  // sequence (or a hold from a release) any chunk the ring may still have in flight, up to the last
  // retired + ring slots - 1; after drain(), none (kept until the ledger goes). Any thread (the encode
  // thread, or the finish thread from a release); the refused path alone takes the lock.
  template <class F>
  void
  refuse(uint64_t seq, F &&add) {
    uint64_t retired = retired_.load(std::memory_order_acquire);
    uint64_t until = seq > retired ? seq : retired == kDrained ? kDrained : retired + ring_slots_ - 1;
    std::lock_guard lock(refused_mutex_);
    auto it = std::find_if(refused_.rbegin(), refused_.rend(), [&](const RingSlot &r) { return r.seq == until; });
    RingSlot &bucket = it != refused_.rend() ? *it : refused_.emplace_back();
    bucket.seq = until;
    add(bucket);
    refused_holds_.fetch_add(1, std::memory_order_relaxed);
  }

  // gw2 (#113): recovery (finish thread): chunks retired + 1 .. seq completed; releases their ring slots'
  // holds in sequence order and resumes the ring at seq + 1. The holds are moved out before retired_
  // moves (until then the encode thread can't claim those slots: they belong to unretired chunks), and
  // released after (a hold made by a release is refused, as in retire()). Returns the chunks released.
  uint32_t
  retireThrough(uint64_t retired, uint64_t seq) {
    std::vector<RingSlot> done;
    uint64_t first = seq - retired > ring_slots_ ? seq - ring_slots_ + 1 : retired + 1;
    for (uint64_t s = first; s <= seq; s++) {
      auto &slot = slots_[s % ring_slots_];
      if (slot.seq > retired && slot.seq <= seq) {
        RingSlot &taken = done.emplace_back();
        taken.seq = slot.seq;
        taken.splits.swap(slot.splits);
        taken.objects.swap(slot.objects);
        taken.presenters.swap(slot.presenters);
        taken.spatial_scalers.swap(slot.spatial_scalers);
        taken.temporal_scalers.swap(slot.temporal_scalers);
      }
    }
    retired_.store(seq, std::memory_order_release);
    for (auto &slot : done)
      release(slot);
    return uint32_t(done.size());
  }

  // the refused holds whose last chunk is at or before `through` (all: kDrained), in the order they were
  // refused, each chunk's in the cross-category order. Moved out first: a hold made by one of these
  // releases is refused anew. gw2 (#113): returns how many were released.
  size_t
  releaseRefusedThrough(uint64_t through) {
    std::vector<RingSlot> done;
    {
      std::lock_guard lock(refused_mutex_);
      auto keep = std::stable_partition(refused_.begin(), refused_.end(),
                                        [&](const RingSlot &r) { return r.seq > through; });
      done.assign(std::make_move_iterator(keep), std::make_move_iterator(refused_.end()));
      refused_.erase(keep, refused_.end());
    }
    size_t n = 0;
    for (auto &slot : done) {
      n += count(slot);
      release(slot);
    }
    refused_holds_.fetch_sub(n, std::memory_order_relaxed);
    return n;
  }
  void
  releaseRefused() {
    releaseRefusedThrough(kDrained);
  }

  std::vector<RingSlot> slots_;
  // refused holds, kept until drain(); gw2 (#113): per last chunk that may use them (RingSlot::seq), so a
  // recovering queue can release them once that chunk completed
  std::vector<RingSlot> refused_;
  std::mutex refused_mutex_;
  std::atomic<size_t> refused_holds_{0};
  bool after_refusal_ = false; // gw2 (#113): finish thread: retireOrRecover's last call was refused
  const uint32_t ring_slots_;
  RetentionPolicy policy_;
  const CompletionIssuer *issuer_ = nullptr; // gw2 (#91)
  std::atomic<uint64_t> retired_{0};
};

} // namespace dxmt::gw2
