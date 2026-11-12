#pragma once

// gw2 (#77, Q1): commit records (architecture council 2026-11-04 §2; ADR 0002 in docs/gw2/adr/).
//
// A chunk commits one command buffer or several (the present split commits the frame's work first;
// on Metal 4 queue events commit too). Once they completed, the finish thread describes each of them
// in one commit record (its ordinal in commit order, its GPU span if Metal gave one, its error, its
// logs) and reports the chunk from the records alone, whichever backend produced them:
//
// - Metal 3 builds them from the chunk's command buffers (recordMetal3Commit): it waits for a buffer
//   that hasn't completed, reads its error only if it failed, its logs if it failed or is the chunk's
//   last buffer (as upstream's line does), and its GPU times only when asked (DXMT_STATS: each is a
//   bridge call).
// - Metal 4 converts its commits' feedback (recordMetal4Commit), which carries no logs.
// - reportCompletedChunk writes what both finish paths wrote before: the device-error lines (split
//   buffers' through the throttle), the GPU line's spans and PRESENT's aftergpu, with the same values
//   in the same order. Each backend keeps its line order (ReportOrder).
//
// A missing GPU time is std::nullopt, not 0; the consumers that took 0 for it still get 0
// (value_or(0)). A record's error is the backend's object, valid until the chunk retires (Metal 3: the
// command buffer owns it; Metal 4: Mtl4Queue::retire releases it), like the record itself.
//
// Generic over the backend's types so the native test can drive it with scripted command buffers
// and commits (tests/gw2/commit_record_test.cpp); production instantiates it in
// dxmt_command_queue.cpp. Finish thread only.

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dxmt::gw2 {

// One committed command buffer of a chunk, once it completed. Error: the backend's error object
// (default-constructed and false when there is none).
template <class Error> struct BasicCommitRecord {
  uint32_t ordinal = 0;                   // its place in the chunk's commit order, from 0
  std::optional<uint64_t> gpu_start_ns;   // GPU execution start (CACurrentMediaTime's clock), if known
  std::optional<uint64_t> gpu_end_ns;     // and end
  Error error{};                          // why the GPU failed it, or none
  std::vector<std::string> logs;          // Metal 3 only: the command buffer's logs, as text
};

// A GPU time in ns, where Metal's 0 means none
inline std::optional<uint64_t>
knownTime(uint64_t ns) {
  return ns ? std::optional<uint64_t>(ns) : std::nullopt;
}

// Metal 3: the record of `cmdbuf`, ordinal `ordinal` of its chunk; `last`: the chunk's last command
// buffer (its logs are read even without an error); `timing`: read its GPU times. `out` is reused
// (its logs keep their capacity). CommandBuffer: status() comparable to CommandBuffer::kScheduled and
// kError (WMTCommandBufferStatus), waitUntilCompleted(), error(), logs(std::vector<std::string> &)
// (appends), gpuStartTime() and gpuEndTime() in ns.
template <class CommandBuffer, class Record>
void
recordMetal3Commit(CommandBuffer &cmdbuf, uint32_t ordinal, bool last, bool timing, Record &out) {
  out.ordinal = ordinal;
  auto status = cmdbuf.status();
  if (status <= CommandBuffer::kScheduled) {
    cmdbuf.waitUntilCompleted();
    status = cmdbuf.status();
  }
  bool failed = status == CommandBuffer::kError;
  // Metal sets a failed command buffer's error (MTLCommandBufferStatusError)
  out.error = failed ? cmdbuf.error() : decltype(out.error){};
  out.logs.clear();
  if (last || failed)
    cmdbuf.logs(out.logs);
  if (timing) {
    out.gpu_start_ns = knownTime(cmdbuf.gpuStartTime());
    out.gpu_end_ns = knownTime(cmdbuf.gpuEndTime());
  } else {
    out.gpu_start_ns.reset();
    out.gpu_end_ns.reset();
  }
}

// gw2 (#78): Metal 3: the records of a whole chunk into `out`, in commit order (its `splits` split
// buffers, then its last buffer). buffer_at(i) gives split buffer i, buffer_at(splits) the last one.
// They are waited for and read as the finish thread always did: the last buffer first, then the
// split buffers in commit order.
template <class BufferAt, class Record>
void
recordMetal3Chunk(size_t splits, BufferAt &&buffer_at, bool timing, std::vector<Record> &out) {
  out.resize(splits + 1);
  {
    auto last = buffer_at(splits);
    recordMetal3Commit(last, uint32_t(splits), true, timing, out.back());
  }
  for (size_t i = 0; i < splits; i++) {
    auto split = buffer_at(i);
    recordMetal3Commit(split, uint32_t(i), false, timing, out[i]);
  }
}

// Metal 4: the record of `commit` (an Mtl4Commit whose feedback is written), ordinal `ordinal`.
// Its times are known when the feedback has them (gpu_start/gpu_end > 0), in Mtl4Commit's ns.
template <class Commit, class Record>
void
recordMetal4Commit(const Commit &commit, uint32_t ordinal, Record &out) {
  out.ordinal = ordinal;
  out.gpu_start_ns = commit.feedback.gpu_start > 0 ? std::optional<uint64_t>(commit.gpuStartNs()) : std::nullopt;
  out.gpu_end_ns = commit.feedback.gpu_end > 0 ? std::optional<uint64_t>(commit.gpuEndNs()) : std::nullopt;
  out.error = commit.error();
  out.logs.clear();
}

// gw2 (#224): which of a chunk's records holds its present pass, for PRESENT's drawwait and early.
// The encode thread notes it with DXMT_STATS: kPresentLastCommit on Metal 3 (the present buffer is the
// chunk's last) and with DXMT_MTL4_PRESENT=mtl3 (the Metal 3 buffer's record follows every commit,
// #227), the pass's commit ordinal on Metal 4 (an empty commit for timestamps may follow it).
constexpr uint32_t kNoPresentCommit = UINT32_MAX;
constexpr uint32_t kPresentLastCommit = UINT32_MAX - 1;

// The record `present_commit` names, or null (none noted, or past the chunk's records)
template <class Record>
const Record *
presentPassRecord(std::span<const Record> records, uint32_t present_commit) {
  if (records.empty() || present_commit == kNoPresentCommit)
    return nullptr;
  if (present_commit == kPresentLastCommit)
    return &records.back();
  return present_commit < records.size() ? &records[present_commit] : nullptr;
}

// gw2 (#10, #32): whether a split buffer's GPU error gets its line: the first 8, then at most one a
// second, each with the count of those not logged since the last line (a GPU fault tends to repeat
// every frame). One per command queue.
template <class TimePoint> class BasicSplitErrorThrottle {
public:
  // Counts one more error at `now`; true if it gets a line, with `not_logged` set
  bool
  line(TimePoint now, uint64_t &not_logged) {
    errors_++;
    if (errors_ > 8 && now - last_line_ < std::chrono::seconds(1))
      return false;
    not_logged = errors_ - logged_ - 1;
    logged_ = errors_;
    last_line_ = now;
    return true;
  }

  // every split error so far, logged or not
  uint64_t
  errors() const {
    return errors_;
  }

private:
  uint64_t errors_ = 0;
  uint64_t logged_ = 0; // errors_ at the last line
  TimePoint last_line_{};
};

// The device-error lines' order within a chunk, each backend's as before: Metal 3 logs the last
// buffer's error and logs first, then the split buffers'; Metal 4 logs in commit order.
enum class ReportOrder : uint8_t { LastFirst, CommitOrder };

// The finish thread's report of a completed chunk from its records (commit order; the last one is
// the chunk's last command buffer, the ones before it the split buffers). Nothing for no records.
// With `stats`: every record's span for the GPU line, then the last one's end for PRESENT's aftergpu.
// Sink: now() (only for an error), deviceError(record), lastLog(record, text),
// splitError(record, index, count, errors, not_logged) (writes the line and the record's logs),
// gpuSpan(start_ns, end_ns), presentGpuEnd(end_ns), the times 0 when unknown.
template <class Record, class Throttle, class Sink>
void
reportCompletedChunk(std::span<const Record> records, ReportOrder order, bool stats, Throttle &throttle, Sink &sink) {
  if (records.empty())
    return;
  size_t splits = records.size() - 1;
  const Record &last = records.back();
  auto last_lines = [&] {
    if (last.error)
      sink.deviceError(last);
    for (auto &log : last.logs)
      sink.lastLog(last, log);
  };
  if (order == ReportOrder::LastFirst)
    last_lines();
  for (size_t i = 0; i < splits; i++) {
    const Record &split = records[i];
    if (__builtin_expect(bool(split.error), 0)) {
      uint64_t not_logged;
      if (throttle.line(sink.now(), not_logged))
        sink.splitError(split, i, splits, throttle.errors(), not_logged);
    }
  }
  if (order == ReportOrder::CommitOrder)
    last_lines();
  if (stats) {
    for (const Record &record : records)
      sink.gpuSpan(record.gpu_start_ns.value_or(0), record.gpu_end_ns.value_or(0));
    sink.presentGpuEnd(last.gpu_end_ns.value_or(0));
  }
}

} // namespace dxmt::gw2
