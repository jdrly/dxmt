#pragma once

// gw2 (#283): which of the three presents a frame asks for, picked once: after a minimum duration
// (`after` > 0), else with a deadline (`deadline` > 0), else a plain present. The Metal 4 branch of
// the context's present case (dxmt_context.cpp) uses it for both of its presents: the drawable's own
// (the Metal 4 order) and a Metal 3 command buffer's (DXMT_MTL4_PRESENT=mtl3).

#include "winemetal.h"

namespace dxmt::gw2 {

struct PresentChoice {
  WMTDrawablePresentMode mode;
  double value; // the minimum duration or the deadline in seconds; 0 for a plain present
};

constexpr PresentChoice
presentChoice(double after, double deadline) {
  if (after > 0)
    return {WMTDrawablePresentAfterMinimumDuration, after};
  if (deadline > 0)
    return {WMTDrawablePresentWithDeadline, deadline};
  return {WMTDrawablePresent, 0};
}

// The present of `choice` on a Metal 3 command buffer (WMT::CommandBuffer, or a test's stand-in)
template <class CommandBuffer, class Drawable>
void
presentThrough(CommandBuffer &cmdbuf, Drawable drawable, PresentChoice choice) {
  switch (choice.mode) {
  case WMTDrawablePresentAfterMinimumDuration:
    cmdbuf.presentDrawableAfterMinimumDuration(drawable, choice.value);
    break;
  case WMTDrawablePresentWithDeadline:
    cmdbuf.presentDrawableWithDeadline(drawable, choice.value);
    break;
  default:
    cmdbuf.presentDrawable(drawable);
    break;
  }
}

} // namespace dxmt::gw2
