#pragma once

// gw2 (#314): DXMT_STEADY_SLEEP. The frame limiter's and the background throttle's deadline
// arithmetic (d3d11_swapchain.cpp's LimitFrameRate and ThrottleBackground), on a clock the caller
// reads (gw2::nowNs: QueryPerformanceCounter). The caller sleeps for what step() returns, so a
// wall clock that steps back can't lengthen a sleep. The same steps as the wall-clock code:
// a fixed cadence from the previous deadline, and a frame that overran restarts it.
//
// Pure C++: tests/gw2/cadence_test.cpp builds it natively.

#include <cstdint>

namespace dxmt::gw2 {

class Cadence {
public:
  // The limiter: interval_ns <= 0 turns it off; the first call with an interval only arms it.
  // Returns how long to sleep before the frame, in ns (0: not at all).
  int64_t
  step(int64_t now_ns, int64_t interval_ns) {
    if (interval_ns <= 0 || !armed_) {
      armed_ = interval_ns > 0;
      deadline_ns_ = now_ns;
      return 0;
    }
    deadline_ns_ += interval_ns;
    if (deadline_ns_ <= now_ns) {
      deadline_ns_ = now_ns;
      return 0;
    }
    return deadline_ns_ - now_ns;
  }
  // The throttle's state change: the cadence starts over from now.
  void
  restart(int64_t now_ns) {
    armed_ = true;
    deadline_ns_ = now_ns;
  }

private:
  int64_t deadline_ns_ = 0;
  bool armed_ = false;
};

} // namespace dxmt::gw2
