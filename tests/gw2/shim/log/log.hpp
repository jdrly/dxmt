#pragma once
#include <sstream>
#include <string>
namespace dxmt {
struct Logger {
  static void info(const std::string &) {}
  static void warn(const std::string &) {}
};
namespace str {
template <typename... T>
std::string
format(const T &...args) {
  std::ostringstream s;
  (s << ... << args);
  return s.str();
}
} // namespace str
} // namespace dxmt
