#pragma once
#include "d3d11_4.h"
#include "thread.hpp"
#include "util_likely.hpp"
#include <atomic>

namespace dxmt {

class d3d11_device_mutex {
public:
  // gw2 (#149): inline up to an uncontended acquire, the thread id read from the TEB; the spin on
  // a lock another thread holds is out of line
  void lock() {
    if (!forced_ && !protected_) // gw2 (#4): forced_
      return;
    uint32_t tid = this_thread::get_id();
    uint32_t expected = 0;
    if (!owner_.compare_exchange_weak(expected, tid, std::memory_order_acquire) && expected != tid)
      lock_contended(tid);
    counter_++;
  }
  void unlock() noexcept {
    if (!forced_ && !protected_) // gw2 (#4): forced_
      return;
    counter_--;
    if (likely(counter_ == 0))
      owner_.store(0, std::memory_order_release);
  }

  bool set_protected(bool Protected);
  bool get_protected();

  // gw2 (#4): the lock excludes from now on whatever the game's multithread protection says, so
  // another thread of DXMT (the DXMT_PROMOTE_FLUSH_MIN watchdog) can enter the immediate context.
  // Call it before the device is visible to the game; it can't be undone.
  void force_locking() { forced_ = true; }

private:
  void lock_contended(uint32_t tid);

  bool protected_ = false;
  bool forced_ = false; // gw2 (#4)
  std::atomic_uint32_t owner_ = 0;
  uint32_t counter_ = 0;
};

class D3D11Multithread : public ID3D11Multithread {
public:
  D3D11Multithread(IUnknown *container, d3d11_device_mutex &mutex)
      : container_(container), mutex_(mutex) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() final { return container_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() final { return container_->Release(); }

  void STDMETHODCALLTYPE Enter() final { mutex_.lock(); }

  void STDMETHODCALLTYPE Leave() final { mutex_.unlock(); }

  WINBOOL STDMETHODCALLTYPE SetMultithreadProtected(WINBOOL Enable) final {
    return mutex_.set_protected(Enable);
  }

  WINBOOL STDMETHODCALLTYPE GetMultithreadProtected() final {
    return mutex_.get_protected();
  }

private:
  IUnknown *container_;
  d3d11_device_mutex &mutex_;
};

} // namespace dxmt
