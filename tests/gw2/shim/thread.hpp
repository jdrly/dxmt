#pragma once
// gw2 (X2, architecture council): native tests' threads. It shadows src/util/thread.hpp:
// dxmt::thread, dxmt::mutex and dxmt::condition_variable are std::'s, and thread priorities are
// no-ops. It tests coordination, not Wine's scheduling; production keeps CreateThread, its
// priorities and its concurrency policy.
#include <condition_variable>
#include <mutex>
#include <thread>

namespace dxmt {
using thread = std::thread;
using mutex = std::mutex;
using condition_variable = std::condition_variable;
} // namespace dxmt

#ifndef THREAD_PRIORITY_TIME_CRITICAL
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_LOWEST (-2)
inline void *
GetCurrentThread() {
  return nullptr;
}
inline int
SetThreadPriority(void *, int) {
  return 1;
}
#endif
