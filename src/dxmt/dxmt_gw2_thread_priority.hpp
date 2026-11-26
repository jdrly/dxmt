#pragma once

// gw2 (#151, #152): the priority a DXMT thread sets at its start, one switch per group of threads
// (DXMT_THREAD_PRIORITY: the encode and finish threads; DXMT_COMPILE_PRIORITY: the compile workers).
//
// - critical (the default, upstream's): THREAD_PRIORITY_TIME_CRITICAL, which this engine's
//   wineserver turns into timeshare off and Mach importance 32, above every GW2 thread, so the
//   thread always gets S cores;
// - normal: THREAD_PRIORITY_NORMAL, timeshare below the game thread;
// - utility: normal, then base priority 20 (bridge call WMTSetThreadBasePrioritySelf), which macOS
//   keeps on the P cores.
//
// Anything else is critical, with a warning.

#include "dxmt_gw2_switches.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace dxmt::gw2 {

enum class ThreadPriority : uint8_t { Critical, Normal, Utility };

// A switch of the grammar above, declared at namespace scope like sw::Switch. The startup line shows
// the mode, utility as utility:ok, utility:failed (one of its threads didn't get base priority 20)
// or utility:pending (none has tried yet).
class ThreadPrioritySwitch {
public:
  explicit ThreadPrioritySwitch(const char *name);
  ThreadPrioritySwitch(const ThreadPrioritySwitch &) = delete;
  ThreadPrioritySwitch &operator=(const ThreadPrioritySwitch &) = delete;

  // The mode now (one environment read), with a warning for a value it doesn't know.
  ThreadPriority read() const;
  // On the thread `thread_name` itself, at its start: the Win32 priority first, then for utility the
  // base priority, which must come after the wineserver's policies for THREAD_PRIORITY_NORMAL. A
  // failure leaves the thread where the step before put it. One ThreadPriority line per thread
  // unless critical.
  void apply(ThreadPriority mode, const char *thread_name);

private:
  sw::Switch<std::string> switch_;
  std::atomic<int> utility_outcome_{0}; // bit 0: a thread got base priority 20; bit 1: one didn't
  sw::Effective line_;
};

} // namespace dxmt::gw2
