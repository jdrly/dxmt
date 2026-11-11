#include "dxmt_gw2_thread_priority.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "util_win32_compat.h"
#include "winemetal.h"

namespace dxmt::gw2 {

namespace {

std::string
parseMode(const std::string &raw, const std::string &fallback) {
  return raw == "critical" || raw == "normal" || raw == "utility" ? raw : fallback;
}

} // namespace

ThreadPrioritySwitch::ThreadPrioritySwitch(const char *name) :
    switch_(name, "critical", parseMode),
    line_(name, [this] {
      auto mode = switch_();
      if (mode != "utility")
        return mode;
      int outcome = utility_outcome_.load(std::memory_order_relaxed);
      return std::string(outcome & 2 ? "utility:failed" : outcome & 1 ? "utility:ok" : "utility:pending");
    }) {}

ThreadPriority
ThreadPrioritySwitch::read() const {
  auto v = switch_.requested();
  auto mode = switch_.parse(v);
  if (!v.empty() && mode != v)
    WARN(switch_.name(), "=", v, " is not critical, normal or utility; using critical");
  return mode == "normal" ? ThreadPriority::Normal : mode == "utility" ? ThreadPriority::Utility : ThreadPriority::Critical;
}

void
ThreadPrioritySwitch::apply(ThreadPriority mode, const char *thread_name) {
  HANDLE self = GetCurrentThread();
  if (mode == ThreadPriority::Critical) {
    SetThreadPriority(self, THREAD_PRIORITY_TIME_CRITICAL);
    return;
  }
  BOOL ok = SetThreadPriority(self, THREAD_PRIORITY_NORMAL);
  std::string win32 = ok ? std::string("ok") : str::format("error ", GetLastError());
  if (mode == ThreadPriority::Normal) {
    Logger::info(str::format("ThreadPriority: ", thread_name, " normal (SetThreadPriority THREAD_PRIORITY_NORMAL: ", win32, ")"));
    return;
  }
  int old_base = -1, new_base = -1;
  int err = WMTSetThreadBasePrioritySelf(20, &old_base, &new_base);
  utility_outcome_.fetch_or(!err && new_base == 20 ? 1 : 2, std::memory_order_relaxed);
  Logger::info(str::format(
      "ThreadPriority: ", thread_name, " utility, base priority ", old_base, " -> ", new_base,
      " (SetThreadPriority THREAD_PRIORITY_NORMAL: ", win32,
      "; thread_policy_set: ", err ? str::format("error ", err) : std::string("ok"), ")"
  ));
}

} // namespace dxmt::gw2
