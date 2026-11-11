#pragma once

// gw2 (#156): when the presenter asks the bridge for its layer's screen values again
// (WMTQueryDisplaySettingForLayer: the layer's view, window and screen, the screen's device
// description and two EDR values, about 28 µs a call). At most once per interval, and at the next
// Present after invalidate() (a resize). An interval of 0 asks at every Present, as before #156.
//
// Pure C++: tests/gw2/layer_poll_test.cpp builds it natively.

#include <cstdint>

namespace dxmt::gw2 {

class LayerPropsPoll {
public:
  explicit LayerPropsPoll(int64_t interval_ns) : interval_ns_(interval_ns) {}

  // Whether to ask now (`now_ns` on a monotonic clock). A true answer starts a new interval.
  bool
  due(int64_t now_ns) {
    if (asked_ && interval_ns_ > 0 && now_ns - asked_at_ < interval_ns_)
      return false;
    asked_ = true;
    asked_at_ = now_ns;
    return true;
  }

  // The next due() asks, whatever the interval.
  void
  invalidate() {
    asked_ = false;
  }

private:
  int64_t interval_ns_;
  int64_t asked_at_ = 0;
  bool asked_ = false;
};

} // namespace dxmt::gw2
