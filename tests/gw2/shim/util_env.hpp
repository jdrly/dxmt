#pragma once
#include <cstdlib>
#include <string>
namespace dxmt::env {
// Every variable unset, unless a test builds with GW2_TEST_REAL_ENV to read switches from its
// environment as DXMT does (presenter_layer_set_test's lifetime-check build)
inline std::string
getEnvVar([[maybe_unused]] const char *name) {
#ifdef GW2_TEST_REAL_ENV
  const char *value = std::getenv(name);
  return value ? value : "";
#else
  return "";
#endif
}
} // namespace dxmt::env
