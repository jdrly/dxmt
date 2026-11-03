#include "dxmt_gw2_control.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

namespace dxmt::gw2 {

namespace {

// gw2 (#7): the file is small; anything past this is ignored
constexpr size_t kMaxFileBytes = 4096;
constexpr double kMaxDeadlineMs = 1000.0;

std::atomic<int32_t> g_present_deadline_us{kControlUnset};

std::string
trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r");
  if (b == std::string::npos)
    return {};
  size_t e = s.find_last_not_of(" \t\r");
  return s.substr(b, e - b + 1);
}

// The poll thread's own state: what it has warned about, so each warning is logged once.
struct Poller {
  std::string path;
  int readable = -1; // -1 not read yet, 0 can't be read, 1 read
  std::set<std::string> warned;

  void
  warnOnce(const std::string &key, const std::string &message) {
    if (warned.insert(key).second)
      Logger::warn(message);
  }

  // false: the value isn't understood (warned once); `us` keeps kControlUnset
  bool
  parseDeadline(const std::string &value, int32_t &us) {
    if (value == "default") {
      us = kControlUnset;
      return true;
    }
    if (value == "refresh") {
      us = -1;
      return true;
    }
    char *end = nullptr;
    double ms = std::strtod(value.c_str(), &end);
    if (!value.empty() && end && *end == '\0' && std::isfinite(ms) && ms >= 0 && ms <= kMaxDeadlineMs) {
      if (ms == 0) {
        us = 0;
      } else {
        long long v = std::llround(ms * 1000.0);
        us = int32_t(v < 1 ? 1 : v);
      }
      return true;
    }
    warnOnce(
        "present_deadline=" + value,
        std::format(
            "Control: present_deadline={} isn't refresh, default, 0 or a number of ms up to {:.0f}; ignored (the "
            "line counts as missing)",
            value, kMaxDeadlineMs
        )
    );
    return false;
  }

  void
  poll() {
    std::string text;
    std::ifstream file(str::topath(path.c_str()).c_str(), std::ios::binary);
    bool ok = bool(file);
    if (ok) {
      text.resize(kMaxFileBytes);
      file.read(text.data(), text.size());
      text.resize(size_t(file.gcount()));
      ok = !file.bad();
    }
    if (int(ok) != readable) {
      if (ok)
        Logger::info(std::format("Control: reading {}", path));
      else
        Logger::warn(std::format("Control: {} can't be read; every key at its default", path));
      readable = int(ok);
    }

    int32_t deadline_us = kControlUnset;
    size_t pos = 0;
    while (ok && pos < text.size()) {
      size_t nl = text.find('\n', pos);
      std::string line = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
      pos = nl == std::string::npos ? text.size() : nl + 1;
      if (line.empty() || line[0] == '#')
        continue;
      size_t eq = line.find('=');
      std::string key = trim(line.substr(0, eq));
      std::string value = eq == std::string::npos ? std::string() : trim(line.substr(eq + 1));
      if (key == "present_deadline" && eq != std::string::npos) {
        int32_t us = kControlUnset;
        if (parseDeadline(value, us))
          deadline_us = us; // the last valid line wins
        continue;
      }
      if (key == "present_deadline")
        warnOnce("present_deadline", "Control: present_deadline without =value; ignored");
      else
        warnOnce("key:" + key, std::format("Control: unknown key \"{}\" (line \"{}\"); ignored", key, line));
    }
    g_present_deadline_us.store(deadline_us, std::memory_order_relaxed);
  }

  [[noreturn]] void
  run() {
    env::setThreadName("dxmt-control");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    while (true) {
      poll();
      Sleep(1000);
    }
  }
};

} // namespace

const std::string &
controlPath() {
  static const std::string path = env::getEnvVar("DXMT_CONTROL");
  return path;
}

void
controlStart() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (controlPath().empty())
      return;
    Logger::info(std::format(
        "Control: polling {} once a second (DXMT_CONTROL); keys: present_deadline=refresh|<ms>|0|default",
        controlPath()
    ));
    dxmt::thread([] {
      // never destroyed: the thread runs until the process exits
      auto *poller = new Poller{controlPath()};
      poller->run();
    }).detach();
  });
}

int32_t
controlPresentDeadlineUs() {
  return g_present_deadline_us.load(std::memory_order_relaxed);
}

} // namespace dxmt::gw2
