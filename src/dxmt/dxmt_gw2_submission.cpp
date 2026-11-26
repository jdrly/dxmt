#include "dxmt_gw2_submission.hpp"
#include "dxmt_gw2_mtl4_queue.hpp"
#include "log/log.hpp"
#include <utility>

namespace dxmt {

namespace {

// gw2 (#77): a Metal 3 command buffer as gw2::recordMetal3Commit reads it
struct Metal3Buffer {
  static constexpr auto kScheduled = WMTCommandBufferStatusScheduled;
  static constexpr auto kError = WMTCommandBufferStatusError;
  WMT::CommandBuffer cmdbuf;

  WMTCommandBufferStatus
  status() {
    return cmdbuf.status();
  }
  void
  waitUntilCompleted() {
    cmdbuf.waitUntilCompleted();
  }
  WMT::Error
  error() {
    return cmdbuf.error();
  }
  void
  logs(std::vector<std::string> &out) {
    if (auto logs = cmdbuf.logs())
      for (auto &log : logs.elements())
        out.push_back(log.description().getUTF8String());
  }
  uint64_t
  gpuStartTime() {
    return cmdbuf.gpuStartTime();
  }
  uint64_t
  gpuEndTime() {
    return cmdbuf.gpuEndTime();
  }
};

} // namespace

// --- Metal 3

Metal3Submission::Metal3Submission(uint32_t ring_slots, Hooks hooks) : hooks_(std::move(hooks)), last_(ring_slots) {}

void
Metal3Submission::begin(uint64_t seq) {
  encoding_ = seq;
  slot(seq) = hooks_.new_command_buffer();
}

void
Metal3Submission::waitEvent(WMT::Event event, uint64_t value) {
  slot(encoding_).encodeWaitForEvent(event, value);
}

void
Metal3Submission::signalEvent(WMT::Event event, uint64_t value) {
  slot(encoding_).encodeSignalEvent(event, value);
}

void
Metal3Submission::encode(uint64_t seq, ChunkEncoder &encoder) {
  // the present split may commit this one and leave the chunk's last one here
  encoder.encode(slot(seq));
}

void
Metal3Submission::commit(uint64_t seq) {
  hooks_.before_commit(); // gw2 (#29): the set's pending adds and removes, if any
  slot(seq).commit();
}

// gw2 (#77): the records of the chunk's last command buffer and of the buffers the present split
// committed first (gw2 #10; held in the retirement ledger, committed earlier on this queue, so done or
// about to be). A buffer still pending is waited for here, never on the encode thread, the last one
// first as before. GPU times only with `timing` (DXMT_STATS).
CompletedChunk
Metal3Submission::wait(uint64_t seq, uint64_t, bool timing) {
  auto splits = hooks_.split_buffers(seq);
  WMT::CommandBuffer last = slot(seq);
  gw2::recordMetal3Chunk(
      splits.size(), [&](size_t i) { return Metal3Buffer{i < splits.size() ? WMT::CommandBuffer(splits[i]) : last}; },
      timing, records_
  );
  // upstream's order: the last buffer's error and logs, then the split buffers'
  return issue(seq, records_, gw2::ReportOrder::LastFirst);
}

void
Metal3Submission::retireChunk(const CompletedChunk &chunk) {
  // the chunk released its last command buffer when it reset; forget the borrowed handle
  slot(chunk.seq()) = {};
}

// gw2 (#29, #31): with the residency set, a chunk's references are what keep its allocations in the set
// (and alive: the command buffers no longer retain them through useResource); with DXMT_UNRETAINED=1
// the command buffers retain nothing, and DXMT_LIFETIME_CHECK=1's stamps assume it. The finish thread
// stopped without retiring every committed chunk, so wait for their command buffers first; the
// split-off ones were committed earlier on this queue and complete before their chunk's last.
// Otherwise the command buffers retain what they use, and the chunks hold only the split buffers, which
// a committed command buffer doesn't need. gw2 (#91): either way the chunks' holds may go (the token);
// the borrowed handles are forgotten, as each retire() did, before the chunks release their buffers.
gw2::DrainedQueue
Metal3Submission::drain() {
  if (hooks_.wait_at_drain()) {
    for (auto &cmdbuf : last_) {
      if (!cmdbuf)
        continue;
      auto status = cmdbuf.status();
      if (status == WMTCommandBufferStatusCommitted || status == WMTCommandBufferStatusScheduled)
        cmdbuf.waitUntilCompleted();
    }
  }
  for (auto &cmdbuf : last_)
    cmdbuf = {};
  return drained();
}

// --- Metal 4

Metal4Submission::Metal4Submission(std::unique_ptr<gw2::Mtl4Queue> queue, WMT::SharedEvent completion_event) :
    queue_(std::move(queue)),
    completion_event_(completion_event) {}

Metal4Submission::~Metal4Submission() = default;

void
Metal4Submission::begin(uint64_t) {}

void
Metal4Submission::waitEvent(WMT::Event event, uint64_t value) {
  queue_->waitForEvent(event, value);
}

void
Metal4Submission::signalEvent(WMT::Event event, uint64_t value) {
  queue_->signalEvent(event, value);
}

void
Metal4Submission::encode(uint64_t, ChunkEncoder &encoder) {
  encoder.encode(*queue_);
}

void
Metal4Submission::commit(uint64_t seq) {
  queue_->commit(seq); // the one left open, if any (gw2::Mtl4Encoding::commitOpen)
}

// gw2 (#32): every commit's feedback; then the chunk's completion event, its last queue operation
// (flushCommands' end, Mtl4Encoding::encodeSignalEvent), ordered after every commit and every
// queue-level wait of the chunk. A chunk with no command buffer, or with queue waits or signals after
// its last one, is complete only once it fired: wait for it too before anything of the chunk is
// released. gw2 (#77): then the commits' records (Metal 4 has no command buffer logs). gw2 (#227): with
// DXMT_MTL4_PRESENT=mtl3, the chunk's Metal 3 present buffers follow as its last records, read as
// Metal 3 reads its buffers (`timing`: their GPU times), so the GPU line and PRESENT's aftergpu read
// the present pass.
CompletedChunk
Metal4Submission::wait(uint64_t seq, uint64_t completion_value, bool timing) {
  auto &commits = queue_->wait(seq);
  if (completion_event_.signaledValue() < completion_value) {
    bool warned = false;
    while (!completion_event_.waitUntilSignaledValue(completion_value, 1000)) {
      if (!warned)
        Logger::warn(str::format("Metal 4: chunk ", seq, "'s completion event not signalled after 1 s; still waiting"));
      warned = true;
    }
  }
  auto &presents = queue_->metal3Presents(seq);
  records_.resize(commits.size() + presents.size());
  for (size_t i = 0; i < commits.size(); i++)
    gw2::recordMetal4Commit(*commits[i], uint32_t(i), records_[i]);
  for (size_t i = 0; i < presents.size(); i++) {
    size_t at = commits.size() + i;
    Metal3Buffer buffer{presents[i].cmdbuf};
    gw2::recordMetal3Commit(buffer, uint32_t(at), at + 1 == records_.size(), timing, records_[at]);
  }
  return issue(seq, records_, gw2::ReportOrder::CommitOrder);
}

void
Metal4Submission::retireChunk(const CompletedChunk &chunk) {
  queue_->retire(chunk.seq()); // releases the records' errors, resets the ring slot's allocator
}

gw2::DrainedQueue
Metal4Submission::drain() {
  queue_->waitIdle(); // every committed command buffer (the chunks' attached_cmdbuf is null on Metal 4)
  return drained();
}

} // namespace dxmt
