#pragma once
#include "Metal.hpp"
#include "thread.hpp"
#include "airconv_public.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace dxmt {

constexpr int kDXMTShaderCacheVersion = AIRCONV_VERSION;

class ShaderCache {
public:
  template <typename T> class LockProtected {
  public:
    [[nodiscard]] LockProtected(dxmt::mutex &mutex, T object) : object(object), mutex_(&mutex) {
      mutex_->lock();
    }
    // gw2 (#95): the mutex is already held (a pool slot taken with try_lock)
    [[nodiscard]] LockProtected(dxmt::mutex &mutex, T object, std::adopt_lock_t)
        : object(object), mutex_(&mutex) {}
    [[nodiscard]] LockProtected() : object({}), mutex_(nullptr) {}
    ~LockProtected() {
      if (mutex_) {
        mutex_->unlock();
        mutex_ = nullptr;
      }
      object = {};
    }
    T object;
    LockProtected(const LockProtected &copy) = delete;
    LockProtected(LockProtected &&move) = default;

    operator bool() const {
      return mutex_ != nullptr;
    }

    T *
    operator->() {
      return &object;
    }

  private:
    dxmt::mutex *mutex_;
  };

  static ShaderCache &getInstance(WMTMetalVersion version);

  LockProtected<WMT::CacheWriter>
  getWriter() {
    if (!scache_writer_)
      return {};
    return {scache_writer_mutex_, scache_writer_};
  }

  // gw2 (#95): with DXMT_AIR_READERS=1 (the default) the one reader behind its lock, as upstream; with
  // n > 1 a pool of n read-only connections: the first free one (try_lock, round robin from a rotating
  // start), else a wait for one, chosen by the rotation.
  LockProtected<WMT::CacheReader>
  getReader() {
    if (!scache_reader_)
      return {};
    if (readers_.empty())
      return {scache_reader_mutex_, scache_reader_};
    size_t n = readers_.size();
    size_t start = next_reader_.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < n; i++) {
      auto &slot = *readers_[(start + i) % n];
      if (slot.mutex.try_lock())
        return {slot.mutex, slot.reader, std::adopt_lock};
    }
    auto &slot = *readers_[start % n];
    return {slot.mutex, slot.reader};
  }

  ShaderCache(WMTMetalVersion metal_version);
  ShaderCache(const ShaderCache &copy) = delete;

private:
  WMT::Reference<WMT::CacheWriter> scache_writer_;
  dxmt::mutex scache_writer_mutex_;

  WMT::Reference<WMT::CacheReader> scache_reader_;
  dxmt::mutex scache_reader_mutex_;

  // gw2 (#95): DXMT_AIR_READERS > 1: the pool (scache_reader_ is its first connection)
  struct ReaderSlot {
    WMT::Reference<WMT::CacheReader> reader;
    dxmt::mutex mutex;
  };
  std::vector<std::unique_ptr<ReaderSlot>> readers_;
  std::atomic<size_t> next_reader_{0};
};

} // namespace dxmt