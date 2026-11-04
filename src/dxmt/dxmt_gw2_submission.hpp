#pragma once

// gw2 (#78, Q2): chunk submission (architecture council 2026-11-04 §2; ADR 0002 in docs/gw2/adr/).
//
// One queue-level runtime seam for both backends, chosen per device when the command queue is made:
// a chunk begins, may wait for or signal events at the queue, is encoded, commits; the finish thread
// waits for it (a completed chunk: its commit records, gw2 #77), reports it, and retires it; teardown
// drains. The calls are per chunk, never per draw. Encoding keeps its two compile-time forms
// (flushCommandsImpl over a WMT::CommandBuffer or a gw2::Mtl4Encoding): encode() hands the chunk's
// encoder the backend's own target, one virtual call per chunk.
//
// - Metal3Submission: the chunk's command buffer comes from the command queue (retained or not, as
//   DXMT_UNRETAINED says); the present split commits the earlier ones inside the encoding and hands
//   back the last one; commit() commits it after the residency set's pending changes. wait() builds the
//   records (the last buffer first, then the split buffers, which the retirement ledger holds). The
//   chunk owns its last command buffer and releases it when it resets (before the ledger's holds, as
//   before); the adapter only borrows it until retire(). drain() waits for every committed buffer when
//   the residency set or DXMT_UNRETAINED / the lifetime check need it.
// - Metal4Submission: owns the gw2::Mtl4Queue. Command buffers begin as the encoders need them;
//   commit() commits the one left open; wait() waits for every commit's feedback and for the chunk's
//   completion event (its last queue operation), then converts the feedback into records. retire()
//   resets the ring slot's allocator after the chunk's own reset (its readbacks read the feedback);
//   drain() is waitIdle().
//
// Threads: begin, waitEvent, signalEvent, encode, commit on the encode thread for the chunk being
// encoded; wait, retire on the finish thread, in sequence order, retire with the token of the last
// wait; drain once both have stopped. The command queue's handshake (ready_for_commit, chunk_ongoing)
// orders a ring slot's use between them.

#include "Metal.hpp"
#include "dxmt_gw2_commit_record.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace dxmt {

namespace gw2 {
class Mtl4Queue;
}

// gw2 (#77): one completed command buffer of a chunk, as both finish paths read it
using CommitRecord = gw2::BasicCommitRecord<WMT::Error>;

// gw2 (#78): what wait() returns: a chunk whose commits all completed, with their records in commit
// order (the last one is the chunk's last command buffer). The records belong to the submission and
// stay valid until this token is retired.
class CompletedChunk {
public:
  CompletedChunk(uint64_t seq, std::span<const CommitRecord> records, gw2::ReportOrder order) :
      seq_(seq),
      records_(records),
      order_(order) {}

  CompletedChunk(CompletedChunk &&) = default;
  CompletedChunk &operator=(CompletedChunk &&) = default;
  CompletedChunk(const CompletedChunk &) = delete;
  CompletedChunk &operator=(const CompletedChunk &) = delete;

  uint64_t
  seq() const {
    return seq_;
  }
  std::span<const CommitRecord>
  records() const {
    return records_;
  }
  // the backend's device-error line order (Metal 3: the last buffer first)
  gw2::ReportOrder
  order() const {
    return order_;
  }

private:
  uint64_t seq_;
  std::span<const CommitRecord> records_;
  gw2::ReportOrder order_;
};

// gw2 (#78): the chunk's encoding, in the backend's own form (encode thread, once per chunk)
class ChunkEncoder {
public:
  virtual ~ChunkEncoder() = default;
  // Metal 3: `cmdbuf` is the chunk's open command buffer; the encoding may commit it and continue in
  // a new one (the present split), and leaves the chunk's last one in `cmdbuf`
  virtual void encode(WMT::CommandBuffer &cmdbuf) = 0;
  // Metal 4: the queue the encoding begins and commits the chunk's command buffers on
  virtual void encode(gw2::Mtl4Queue &queue) = 0;
};

class ChunkSubmission {
public:
  virtual ~ChunkSubmission() = default;

  // --- encode thread, chunk `seq`

  // The chunk begins (Metal 3: its command buffer; Metal 4: nothing yet, command buffers begin as the
  // encoders need them)
  virtual void begin(uint64_t seq) = 0;
  // Event operations at the start (wait) or end (signal) of the work encoded so far, each backend's
  // way: Metal 3 in the open command buffer, Metal 4 at the queue (it commits nothing)
  virtual void waitEvent(WMT::Event event, uint64_t value) = 0;
  virtual void signalEvent(WMT::Event event, uint64_t value) = 0;
  virtual void encode(uint64_t seq, ChunkEncoder &encoder) = 0;
  // Commits what the chunk left open, after the residency sets' pending changes
  virtual void commit(uint64_t seq) = 0;

  // --- finish thread, in sequence order

  // Blocks until every commit of chunk `seq` completed (Metal 4: and its completion event reached
  // `completion_value`); `timing`: read the GPU times where that costs (Metal 3, DXMT_STATS)
  virtual CompletedChunk wait(uint64_t seq, uint64_t completion_value, bool timing) = 0;
  // After the report and the chunk's own reset and holds: what the submission keeps of the chunk
  virtual void retire(CompletedChunk &&chunk) = 0;

  // --- teardown, both threads stopped

  // Blocks until the committed chunks no longer run where the backend must wait for that
  virtual void drain() = 0;
};

// gw2 (#78): Metal 3 (production)
class Metal3Submission final : public ChunkSubmission {
public:
  struct Hooks {
    std::function<WMT::CommandBuffer()> new_command_buffer; // a buffer on the queue (DXMT_UNRETAINED's kind)
    void (*before_commit)();                                // the residency set's pending changes (gw2 #29)
    // finish thread: chunk `seq`'s split buffers (the retirement ledger's), in commit order
    std::function<std::span<WMT::Reference<WMT::CommandBuffer>>(uint64_t seq)> split_buffers;
    bool (*wait_at_drain)();                                // the residency set, DXMT_UNRETAINED, the lifetime check
  };

  Metal3Submission(uint32_t ring_slots, Hooks hooks);

  void begin(uint64_t seq) override;
  void waitEvent(WMT::Event event, uint64_t value) override;
  void signalEvent(WMT::Event event, uint64_t value) override;
  void encode(uint64_t seq, ChunkEncoder &encoder) override;
  void commit(uint64_t seq) override;
  CompletedChunk wait(uint64_t seq, uint64_t completion_value, bool timing) override;
  void retire(CompletedChunk &&chunk) override;
  void drain() override;

private:
  WMT::CommandBuffer &
  slot(uint64_t seq) {
    return last_[seq % last_.size()];
  }

  Hooks hooks_;
  // per ring slot: the chunk's open, then last command buffer (borrowed: the chunk owns it), null
  // once retired
  std::vector<WMT::CommandBuffer> last_;
  uint64_t encoding_ = 0;                 // encode thread: the chunk being encoded
  std::vector<CommitRecord> records_;     // finish thread: the last wait's
};

// gw2 (#78): Metal 4 (production): `queue` valid; `completion_event` the command queue's event that
// each chunk signals last (borrowed: it outlives the submission's use of it)
class Metal4Submission final : public ChunkSubmission {
public:
  Metal4Submission(std::unique_ptr<gw2::Mtl4Queue> queue, WMT::SharedEvent completion_event);
  ~Metal4Submission() override;

  void begin(uint64_t seq) override;
  void waitEvent(WMT::Event event, uint64_t value) override;
  void signalEvent(WMT::Event event, uint64_t value) override;
  void encode(uint64_t seq, ChunkEncoder &encoder) override;
  void commit(uint64_t seq) override;
  CompletedChunk wait(uint64_t seq, uint64_t completion_value, bool timing) override;
  void retire(CompletedChunk &&chunk) override;
  void drain() override;

private:
  std::unique_ptr<gw2::Mtl4Queue> queue_;
  WMT::SharedEvent completion_event_;
  std::vector<CommitRecord> records_; // finish thread: the last wait's
};

} // namespace dxmt
