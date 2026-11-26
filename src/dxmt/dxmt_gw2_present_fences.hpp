#pragma once

// gw2 (#353): a present's fences with DXMT_MTL4_PRESENT=mtl3 (ADR 0018).
//
// A present is an encoder of the fence chain like any other. ArgumentEncodingContext::present gives
// it an ID and a fence update, endPass runs its waits through FenceLocalityCheck, and it can become a
// checkpoint. Later encoders then wait on its fence slot, and the simplification drops their waits on
// everything the present's summary covers (collectCompressed in dxmt_deptrack.cpp): waiting on the
// present is taken to mean that what it waited for has finished.
//
// With DXMT_MTL4_PRESENT=mtl3 the pass is in a Metal 3 command buffer, and its fence waits and its
// update are never encoded. A Metal 4 wait on its slot then waits for the update 256 IDs before it,
// and the waits dropped through it order nothing. Two kinds of encoder wait on it:
// - the checkpoint whose window covers it, 64 to 127 IDs after it (every ID is in one window). That
//   checkpoint then doesn't wait for what the present waited for (the passes that wrote the back
//   buffer), and the encoders after it count on it for that;
// - when the present is a checkpoint itself, every encoder up to the next one, for everything its
//   window covers.
// With DXMT_MTL4_SHOWN_WAIT=write the next frame's commits before its first back-buffer write wait for
// no event either, so such an encoder can run while the passes it should wait for still run. That's
// the hole tests/gw2/dependency_compression_test.cpp shows against its fence oracle. In GW2's frames
// (about 92 IDs on the M2 Max's DEPS line, the shown wait at 95% of its encoders) the covering
// checkpoint comes before the shown wait in about a third of the frames by my estimate, so a stale
// wait is the normal case, not a sign of a race. DEPS presentstale gives the real rate.
//
// - presentFencesReachQueue(): whether a present's fence waits and updates reach the Metal 4 queue.
// - PresentFences: the encode thread's state across chunks, and what it counted for the chunk it
//   flushes (PresentFenceChunk). DXMT_MTL4_PRESENT_FENCES=1 encodes the present's fences on the Metal 4
//   queue in an encoder of their own; then no present leaves a hole and nothing is counted.
// - PresentFenceReport: the finish thread's lines at a device error, from the chunks' records.
//
// Pure C++ (dxmt_deptrack.hpp's FenceSet only): the dependency test builds it natively.

#include "dxmt_deptrack.hpp"
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace dxmt::gw2 {

// A present's fence waits and its update reach the Metal 4 queue: the Metal 4 present pass encodes
// them; with mtl3 only DXMT_MTL4_PRESENT_FENCES=1's fence-only encoder does
constexpr bool
presentFencesReachQueue(bool mtl3, bool present_fences) {
  return !mtl3 || present_fences;
}

constexpr uint32_t kNoPosition = UINT32_MAX;

// What one chunk's flush counted, for the finish thread's line at a device error. Positions are in
// the chunk's encoder list (merged-away encoders included).
struct PresentFenceChunk {
  uint32_t encoders = 0;
  uint32_t commits = 0;           // Metal 4 command buffers the chunk began
  uint32_t commit_begin[4] = {};  // the position each of the first four began at
  uint32_t shown_at = kNoPosition; // the chunk's first shown wait of the queue went before this position
  uint64_t shown_value = 0;
  uint64_t present = 0;           // the present's encoder ID, 0 for none
  bool present_mtl3 = false;
  bool present_checkpoint = false;
  bool present_reached = true;    // its fences reached the Metal 4 queue
  uint64_t frame_value = 0;       // mtl3: the value of the Metal 3 buffer's `frame` wait and `shown` signal
  uint32_t stale = 0;             // encoders that waited on an unreached present's fence, unordered by `shown`
  uint64_t stale_present = 0;     // the latest such present's encoder ID in the chunk
};

// The record as key=value fields (docs/gw2/README.md, `Present fences`)
inline std::string
describe(const PresentFenceChunk &c) {
  std::string commits;
  for (uint32_t i = 0; i < c.commits && i < 4; i++)
    commits += std::format("{}{}", i ? "," : "", c.commit_begin[i]);
  if (c.commits > 4)
    commits += ",…";
  if (commits.empty())
    commits = "-";
  std::string shown = c.shown_at == kNoPosition ? std::string("-") : std::format("{}:{}", c.shown_at, c.shown_value);
  std::string present = "-";
  if (c.present)
    present = std::format(
        "{}:{}{},{}", c.present, c.present_mtl3 ? "mtl3" : "mtl4", c.present_checkpoint ? ",checkpoint" : "",
        c.present_reached ? "reached" : "unreached"
    );
  std::string frame = c.present_mtl3 ? std::format(" frame={}", c.frame_value) : std::string();
  std::string stale = c.stale ? std::format("{}:{}", c.stale, c.stale_present) : std::string("0");
  return std::format(
      "encoders={} commits={} shown={} present={}{} stale={}", c.encoders, commits, shown, present, frame, stale
  );
}

// Encode thread, one per context: the presents whose fences didn't reach the Metal 4 queue, each until
// a shown wait orders the queue behind it (every later commit then runs after its Metal 3 buffer,
// which ran after every commit before it) or its fence slot is reused 256 IDs later. A deferred shown
// wait can leave more than one outstanding (a frame that doesn't write the back buffer); the oldest of
// kGaps goes first, uncounted from then on.
class PresentFences {
public:
  explicit PresentFences(bool emit = false) : emit_(emit) {}

  // DXMT_MTL4_PRESENT_FENCES: an mtl3 present's fences go to the Metal 4 queue
  bool
  emit() const {
    return emit_;
  }

  // a chunk's flush begins with `encoders` encoders
  void
  begin(uint32_t encoders) {
    chunk_ = {};
    chunk_.encoders = encoders;
  }

  // an encoder of ID `id` (a render pass: its fragment ID) emits the waits `wait` (and a render pass
  // `vertex`). True when one of them names an unreached present's slot. An encoder without a fence ID
  // (~0: timestamps, queue events) waits on nothing.
  bool
  encoder(EncoderId id, const FenceSet &wait, const FenceSet *vertex = nullptr) {
    if (id == ~0ull)
      return false;
    bool stale = false;
    for (auto &gap : gaps_) {
      if (!gap || id <= gap)
        continue;
      if (id - gap >= kParityLane) {
        gap = 0; // the slot was updated again since, by an encoder whose update reaches the queue
        continue;
      }
      if (wait.test(gap) || (vertex && vertex->test(gap))) {
        stale = true;
        chunk_.stale_present = std::max(chunk_.stale_present, gap);
      }
    }
    if (stale)
      chunk_.stale++;
    return stale;
  }

  // a present at encoder ID `id`; `checkpoint`: it was a checkpoint; `mtl3`: its pass is in a Metal 3
  // command buffer whose `frame` and `shown` values are `value`. True when its fences don't reach the
  // Metal 4 queue.
  bool
  presented(EncoderId id, bool checkpoint, bool mtl3, uint64_t value) {
    chunk_.present = id;
    chunk_.present_mtl3 = mtl3;
    chunk_.present_checkpoint = checkpoint;
    chunk_.present_reached = presentFencesReachQueue(mtl3, emit_);
    chunk_.frame_value = value;
    if (!chunk_.present_reached) {
      // the oldest outstanding one goes when all are taken
      size_t at = 0;
      for (size_t i = 0; i < kGaps; i++) {
        if (!gaps_[i]) {
          at = i;
          break;
        }
        if (gaps_[i] < gaps_[at])
          at = i;
      }
      gaps_[at] = id;
    }
    return !chunk_.present_reached;
  }

  // the queue waits for `shown` at `value` before `position`: every commit after it runs after the
  // Metal 3 buffer of every present up to that value
  void
  shownWaited(uint32_t position, uint64_t value) {
    for (auto &gap : gaps_)
      gap = 0;
    if (chunk_.shown_at == kNoPosition) {
      chunk_.shown_at = position;
      chunk_.shown_value = value;
    }
  }

  // a Metal 4 command buffer of the chunk began at `position`
  void
  commitBegan(uint32_t position) {
    if (chunk_.commits < 4)
      chunk_.commit_begin[chunk_.commits] = position;
    chunk_.commits++;
  }

  const PresentFenceChunk &
  chunk() const {
    return chunk_;
  }

private:
  static constexpr size_t kGaps = 4;
  bool emit_;
  EncoderId gaps_[kGaps] = {}; // the unreached presents still unordered, 0 for none
  PresentFenceChunk chunk_;
};

// Finish thread, one per command queue: at a chunk with a device error, one line with its record and
// the chunk before it; at the chunk after it, one line with that chunk's record. The first 8 error
// chunks get lines.
class PresentFenceReport {
public:
  // chunk `seq` of frame `frame` completed with record `c`; `error`: one of its commits failed.
  // `switch_value`: DXMT_MTL4_PRESENT_FENCES as the startup line shows it. Returns the lines to log.
  std::vector<std::string>
  completed(uint64_t seq, uint64_t frame, const PresentFenceChunk &c, bool error, const char *switch_value) {
    std::vector<std::string> lines;
    if (next_after_) {
      lines.push_back(std::format(
          "Present fences after the device error in chunk {}: chunk {} (frame {}): {}", next_after_, seq, frame,
          describe(c)
      ));
      next_after_ = 0;
    }
    if (error && errors_ < 8) {
      errors_++;
      std::string before = have_prev_ && prev_seq_ + 1 == seq
                               ? std::format("chunk {} (frame {}): {}", prev_seq_, prev_frame_, describe(prev_))
                               : std::string("not recorded");
      lines.push_back(std::format(
          "Present fences at a device error: chunk {} (frame {}): {}; the chunk before, {} "
          "(DXMT_MTL4_PRESENT_FENCES={}; the first 8 are logged)",
          seq, frame, describe(c), before, switch_value
      ));
      next_after_ = seq;
    }
    prev_ = c;
    prev_seq_ = seq;
    prev_frame_ = frame;
    have_prev_ = true;
    return lines;
  }

private:
  PresentFenceChunk prev_;
  uint64_t prev_seq_ = 0, prev_frame_ = 0;
  bool have_prev_ = false;
  uint64_t next_after_ = 0;
  uint32_t errors_ = 0;
};

} // namespace dxmt::gw2
