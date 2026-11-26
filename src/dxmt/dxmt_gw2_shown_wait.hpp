#pragma once

// gw2 (#271): DXMT_MTL4_SHOWN_WAIT=write, with DXMT_MTL4_PRESENT=mtl3 (#227).
//
// With mtl3 the Metal 4 queue waits for the Metal 3 present buffer's `shown` event, so the next frame
// can't write the texture the present pass reads while the pass still reads it. Without this switch
// it waits right after the present, before any of the next frame's work. The Metal 3 buffer waits on
// the GPU for its drawable first, so the next frame's GPU work waits for the drawable too, and each
// frame's GPU work gets one refresh. On 0.2 the drawables come back a refresh late (PRESENT drawwait
// 16.3 ms), which made every frame over 16.7 ms on the GPU a skipped refresh.
//
// With `write` the queue waits only before the first encoder that writes the texture a pending present
// read. A frame's work before that encoder runs while the present pass waits for its drawable.
//
// Both halves run on the encode thread, which records a chunk's encoders and then flushes them:
// - record: ArgumentEncodingContext::present calls presented() with the texture's allocation, and
//   every texture access with ResourceAccess::Write (ArgumentEncodingContext::access, through which
//   every texture write the encoders make is tracked) asks written() whether its encoder gets the
//   flag. An encoder the optimizer merges into another passes its flag on.
// - flush: after a Metal 3 present, flushed() with its `shown` value; before a flagged encoder,
//   waitBefore() gives the value to wait for (0: nothing; the queue already waits for it).
//
// One texture at a time. A present that reads another texture than the present before it (a second
// swap chain, a resize) waits right after it, as without the switch, so a write to the earlier
// texture can never be missed. `shown` only rises, so one wait covers every present before it.
//
// gw2 (#316): ShownRepair, below, keeps a CPU signal for a failed present from leaving `shown` lower.
//
// Pure C++: tests/gw2/shown_wait_test.cpp builds it natively.

#include <cstdint>

namespace dxmt::gw2 {

class ShownWait {
public:
  // record: a present reads `allocation`. true: the flush waits right after it (the first present,
  // or another texture than the last present's)
  bool
  presented(const void *allocation) {
    bool now = allocation != source_;
    source_ = allocation;
    return now;
  }

  // record: an encoder writes `allocation`. true: it gets the flag
  bool
  written(const void *allocation) const {
    return source_ && allocation == source_;
  }

  // flush: a Metal 3 present signals `shown` at `value`; `now`: the queue waits for it right away
  void
  flushed(uint64_t value, bool now) {
    value_ = value;
    if (now)
      waited_ = value;
  }

  // flush: before a flagged encoder, the value to wait for, 0 if the queue already waits for it
  uint64_t
  waitBefore() {
    if (value_ <= waited_)
      return 0;
    waited_ = value_;
    return value_;
  }

  // the source a present read, null before the first
  const void *
  source() const {
    return source_;
  }

private:
  const void *source_ = nullptr;
  uint64_t value_ = 0;  // the last flushed present's `shown` value
  uint64_t waited_ = 0; // the highest `shown` value the queue waits for
};

// gw2 (#316): the CPU's stand-in for a failed Metal 3 present's `shown` signal
// (Mtl4Queue::waitMetal3). The event's setter assigns, and the GPU may signal a later present's
// higher value between this read and this set, so a CPU set can lower `shown`. It can't be made
// atomic against the GPU. Instead, once the CPU has signalled, every later present checks after
// its buffer completed that `shown` is at least its own value. Presents are handled in rising
// order by one thread at a time (the finish thread, then waitIdle), so the present whose signal
// was overwritten always comes after the overwrite. A process without a failed present makes no
// call on the event here.
class ShownRepair {
public:
  // A present's buffer completed. failed: it reported an error, so it may never have signalled.
  // True when this call signalled `value` from the CPU.
  template <class Event>
  bool
  completed(Event &event, uint64_t value, bool failed) {
    if (!failed && !cpu_signalled_)
      return false;
    if (event.signaledValue() >= value)
      return false;
    event.signalValue(value);
    cpu_signalled_ = true;
    return true;
  }

private:
  bool cpu_signalled_ = false;
};

} // namespace dxmt::gw2
