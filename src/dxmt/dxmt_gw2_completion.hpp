#pragma once

// gw2 (#91, L3): completion tokens (architecture council 2026-11-04 §2 PR 3 and §3 L3; ADR 0003 in
// docs/gw2/adr/). GPU completion authorizes retirement: the retirement ledger releases a chunk's holds
// only with the token its queue's chunk submission gave for that chunk, and its teardown drain only
// with the token the submission's drain gave.
//
// - BasicCompletedChunk: what ChunkSubmission::wait(seq) returns once every commit of chunk `seq`
//   completed: the chunk's records in commit order (valid until the submission takes the token back in
//   retire()), and its issuer, the submission that waited. Only an issuer makes one (a
//   CompletionIssuer, which every ChunkSubmission is). Move-only; a moved-from token is empty (no
//   issuer, sequence 0) and authorizes nothing.
// - DrainedQueue: what ChunkSubmission::drain() returns at teardown, once nothing the queue committed
//   still needs what its chunks hold. Move-only, empty once moved from, like the chunk token.
//
// A consumer checks a token's issuer against the one it expects (the ledger: its queue's submission),
// so another queue's token, even for the same sequence, is refused.
//
// Pure (no Metal): the native tests issue tokens from a test issuer.

#include "dxmt_gw2_commit_record.hpp"
#include <cstdint>
#include <span>
#include <utility>

namespace dxmt::gw2 {

class CompletionIssuer;

template <class Record> class BasicCompletedChunk {
public:
  BasicCompletedChunk(BasicCompletedChunk &&o) noexcept :
      issuer_(std::exchange(o.issuer_, nullptr)),
      seq_(std::exchange(o.seq_, 0)),
      records_(std::exchange(o.records_, {})),
      order_(o.order_) {}
  BasicCompletedChunk &
  operator=(BasicCompletedChunk &&o) noexcept {
    issuer_ = std::exchange(o.issuer_, nullptr);
    seq_ = std::exchange(o.seq_, 0);
    records_ = std::exchange(o.records_, {});
    order_ = o.order_;
    return *this;
  }
  BasicCompletedChunk(const BasicCompletedChunk &) = delete;
  BasicCompletedChunk &operator=(const BasicCompletedChunk &) = delete;

  // false once moved from: it authorizes nothing
  explicit operator bool() const {
    return issuer_ != nullptr;
  }
  // the submission whose wait() made it (null: empty)
  const CompletionIssuer *
  issuer() const {
    return issuer_;
  }
  uint64_t
  seq() const {
    return seq_;
  }
  // the chunk's commit records, in commit order (the last one is the chunk's last command buffer)
  std::span<const Record>
  records() const {
    return records_;
  }
  // the backend's device-error line order (Metal 3: the last buffer first)
  ReportOrder
  order() const {
    return order_;
  }

private:
  friend class CompletionIssuer;
  BasicCompletedChunk(const CompletionIssuer *issuer, uint64_t seq, std::span<const Record> records, ReportOrder order) :
      issuer_(issuer),
      seq_(seq),
      records_(records),
      order_(order) {}

  const CompletionIssuer *issuer_;
  uint64_t seq_;
  std::span<const Record> records_;
  ReportOrder order_;
};

class DrainedQueue {
public:
  DrainedQueue(DrainedQueue &&o) noexcept : issuer_(std::exchange(o.issuer_, nullptr)) {}
  DrainedQueue &
  operator=(DrainedQueue &&o) noexcept {
    issuer_ = std::exchange(o.issuer_, nullptr);
    return *this;
  }
  DrainedQueue(const DrainedQueue &) = delete;
  DrainedQueue &operator=(const DrainedQueue &) = delete;

  explicit operator bool() const {
    return issuer_ != nullptr;
  }
  const CompletionIssuer *
  issuer() const {
    return issuer_;
  }

private:
  friend class CompletionIssuer;
  explicit DrainedQueue(const CompletionIssuer *issuer) : issuer_(issuer) {}

  const CompletionIssuer *issuer_;
};

// The only maker of tokens: a chunk submission derives from it and issues them in wait() and drain().
class CompletionIssuer {
public:
  CompletionIssuer(const CompletionIssuer &) = delete;
  CompletionIssuer &operator=(const CompletionIssuer &) = delete;

  // `token` came from this issuer and wasn't moved from
  template <class Record>
  bool
  issued(const BasicCompletedChunk<Record> &token) const {
    return token.issuer() == this;
  }

protected:
  CompletionIssuer() = default;
  ~CompletionIssuer() = default;

  // chunk `seq` completed, with `records` (which the issuer keeps valid until the token comes back)
  template <class Record>
  BasicCompletedChunk<Record>
  completed(uint64_t seq, std::span<const Record> records, ReportOrder order) const {
    return BasicCompletedChunk<Record>(this, seq, records, order);
  }
  // teardown: nothing the queue committed needs its chunks' holds any more
  DrainedQueue
  drained() const {
    return DrainedQueue(this);
  }
};

} // namespace dxmt::gw2
