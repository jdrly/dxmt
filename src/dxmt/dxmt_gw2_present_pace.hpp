#pragma once

// gw2 (#157): DXMT_PRESENT_QUEUE=pace, and the skip arithmetic it shares with the PRESENT section.
//
// - skippedRefreshes / skipWasQueued: the PRESENT section's count on a whole-refresh grid, the
//   refreshes skipped between two consecutive shown frames beyond the target, and whether the later
//   frame was already encoded when the earlier one reached the screen (missed) or not (starved).
//   The section and LiveSkips both call them, so their counts can't disagree.
// - LiveSkips: the same count per frame, as the encode thread collects the presented records, for
//   pace's back-off. The section counts once per 2 s window, too late to back off at once.
// - PresentPace: the render thread's sleep at the end of Present. When the last frame's nextDrawable
//   blocked, it sleeps that long less a margin (at most one refresh less the margin), so the game
//   starts its next frame later and the queue drains. It gives up for the swap chain's life as soon
//   as a refresh is skipped (missed or starved) while it is sleeping: a wrong guess costs a frame.
//
// Pure C++: tests/gw2/present_pace_test.cpp builds it natively, with fake times.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dxmt::gw2 {

// Refreshes skipped between two shown frames `gap_ms` apart, beyond the `vsyncs` the pacing asks for.
inline int64_t
skippedRefreshes(double gap_ms, double refresh_ms, uint32_t vsyncs) {
  return std::max<int64_t>(0, std::llround(gap_ms / refresh_ms) - (int64_t)vsyncs);
}

// Whether a frame encoded at `encode_ns` was queued when the previous shown frame reached the screen
// at `last_presented_ns`: its skipped refreshes are `missed`, otherwise `starved`.
inline bool
skipWasQueued(uint64_t encode_ns, uint64_t last_presented_ns) {
  return encode_ns < last_presented_ns;
}

// The PRESENT section's missed and starved, counted per record on a whole-refresh grid (no frame
// limiter). Encode thread.
class LiveSkips {
public:
  // One presented record (presented_ns 0: never shown). refresh_ms and vsyncs as presentSetPacing
  // gave them; vsyncs 0 or a limiter: no grid, nothing counted.
  void
  add(uint64_t frame, uint64_t encode_ns, uint64_t presented_ns, double refresh_ms, uint32_t vsyncs,
      double limiter_ms) {
    if (!presented_ns) {
      last_valid_ = false; // as the section: no pair across a dropped frame
      return;
    }
    if (refresh_ms > 0 && vsyncs > 0 && limiter_ms <= 0 && last_valid_ && frame > last_frame_ &&
        presented_ns > last_presented_ns_) {
      int64_t skipped = skippedRefreshes((presented_ns - last_presented_ns_) / 1e6, refresh_ms, vsyncs);
      if (skipWasQueued(encode_ns, last_presented_ns_))
        missed_ += skipped;
      else
        starved_ += skipped;
    }
    last_valid_ = true;
    last_frame_ = frame;
    last_presented_ns_ = presented_ns;
  }

  uint64_t missed() const { return missed_; }
  uint64_t starved() const { return starved_; }

private:
  bool last_valid_ = false;
  uint64_t last_frame_ = 0, last_presented_ns_ = 0;
  uint64_t missed_ = 0, starved_ = 0;
};

// pace's state. Render thread, once per Present, after the frame was committed.
class PresentPace {
public:
  static constexpr int64_t kMarginNs = 2'000'000;
  // Skips count against a sleep for this long after it: presented records reach the encode thread a
  // present or two after the frame was shown, about 50 ms after its Present at a full queue.
  static constexpr int64_t kWatchNs = 500'000'000;

  struct Step {
    int64_t sleep_ns = 0;
    bool gave_up = false;           // this step gave up (log it once)
    uint64_t missed = 0, starved = 0; // with gave_up: the rise that did it
    int64_t last_sleep_ns = 0;        // with gave_up: the last sleep before it
  };

  // now_ns: a monotonic clock. blocked_ns and blocked_seq: the last nextDrawable's block and which
  // present it was (a block is answered once, however many Presents read it). refresh_ns: one
  // refresh when the present is paced at every vsync, else 0 (no sleep). missed, starved: the
  // LiveSkips totals.
  Step
  next(int64_t now_ns, int64_t blocked_ns, uint64_t blocked_seq, int64_t refresh_ns, uint64_t missed,
       uint64_t starved) {
    Step s;
    if (gave_up_)
      return s;
    bool watching = slept_ && now_ns - slept_at_ < kWatchNs;
    if (watching && (missed > base_missed_ || starved > base_starved_)) {
      gave_up_ = true;
      s.gave_up = true;
      s.missed = missed - base_missed_;
      s.starved = starved - base_starved_;
      s.last_sleep_ns = last_sleep_ns_;
      return s;
    }
    if (!watching) {
      // not sleeping lately: a skip now isn't pace's (a loading screen, a hitch with an empty queue)
      base_missed_ = missed;
      base_starved_ = starved;
    }
    if (refresh_ns <= 0 || blocked_seq == answered_seq_)
      return s;
    answered_seq_ = blocked_seq;
    int64_t sleep = std::min(blocked_ns, refresh_ns) - kMarginNs;
    if (sleep <= 0)
      return s;
    s.sleep_ns = sleep;
    slept_ = true;
    slept_at_ = now_ns;
    last_sleep_ns_ = sleep;
    return s;
  }

  bool gaveUp() const { return gave_up_; }

private:
  bool gave_up_ = false;
  bool slept_ = false;
  int64_t slept_at_ = 0, last_sleep_ns_ = 0;
  uint64_t answered_seq_ = 0;
  uint64_t base_missed_ = 0, base_starved_ = 0;
};

} // namespace dxmt::gw2
