#include "d3d11_multithread.hpp"
#include "thread.hpp"
#include "util_bit.hpp"
#include "util_likely.hpp"
#include <atomic>

namespace dxmt {

// gw2 (#149): lock()'s contended path; the uncontended one is inline in the header
void d3d11_device_mutex::lock_contended(uint32_t tid) {
  uint32_t expected = 0;
  while(!owner_.compare_exchange_weak(expected, tid, std::memory_order_acquire)) {
    if (unlikely(expected == tid))
      break;
    // better to spin here
    while (owner_.load(std::memory_order_relaxed)) {
#if defined(DXMT_ARCH_X86)
      _mm_pause();
#elif defined(DXMT_ARCH_ARM64)
      __asm__ __volatile__("yield");
#else
// do nothing
#endif
    }
    expected = 0;
  }
}

bool d3d11_device_mutex::set_protected(bool Protected) {
  bool ret = protected_;
  protected_ = Protected;
  // gw2 (#4): a forced lock keeps its owner; turning protection off changes nothing then
  if (ret && !Protected && !forced_) {
    counter_ = 0;
    owner_.store(0, std::memory_order_release);
  }
  return ret;
}
bool d3d11_device_mutex::get_protected() { return protected_; }

}; // namespace dxmt
