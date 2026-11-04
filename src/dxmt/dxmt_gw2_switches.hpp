#pragma once

// gw2 (#60): switches declared by the module that reads them (ADR 0004, docs/gw2/adr/).
//
// - A switch is a `DXMT_*` setting with a fallback and the grammar it has always had. Its module
//   declares it once, at namespace scope: `sw::Switch<bool> kFoo{"DXMT_FOO", true, sw::legacy::notZero};`.
//   The declaration registers it while the DLL loads, so the registry knows every switch before the
//   first startup line, whatever is read first. Declaring reads nothing.
// - A read parses the source (the environment in production) at that moment, as `getEnvVar` did:
//   the switch keeps no cache of its own. A caller that cached its value (a `static const`) keeps
//   that cache, and a caller that read on every call still does, so each read happens when and as
//   often as it always did.
// - The startup line is generated: the caller's fields in their order (today's names and order), a
//   field without a value taking the switch's value from the registry; then every listed switch the
//   caller didn't name, by name; then the caller's trailing fields (`cpus`, `refresh`). A new switch
//   appears on the line by declaring itself.
// - Grammars are not unified: each legacy parser reproduces one existing read exactly (`!= "0"`,
//   `== "1"`, strtoul without a full-consumption check). Changing one would change accepted values.
//
// Pure C++ (no Windows, no Metal): tests/gw2/switches_test.cpp builds it natively against isolated
// registries.

#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace dxmt::sw {

// The requested value of a switch: its raw text, "" when unset (as env::getEnvVar returns it).
using Source = std::function<std::string(const char *name)>;
// A switch's effective value as the startup line shows it.
using EffectiveFn = std::function<std::string()>;

class Registry {
public:
  explicit Registry(Source source);
  Registry(const Registry &) = delete;
  Registry &operator=(const Registry &) = delete;

  // Reads the source now.
  std::string requested(const char *name) const;

  // A switch registers its own value formatter. false (and nothing registered) if `name` already
  // has a switch: one declaration per switch and registry.
  bool add(const char *name, EffectiveFn value, bool listed);
  // A capability outcome or a process's cached value replaces the switch's parsed value on the
  // startup line ("failed", "unsupported"). Without a switch of that name, the provider alone is a
  // listed entry.
  void provideEffective(const char *name, EffectiveFn fn);

  // The effective value of a listed entry, evaluated now; nullopt if there is none.
  std::optional<std::string> effective(const std::string &name) const;
  // The names of the listed entries, by name.
  std::vector<std::string> listed() const;

private:
  struct Entry {
    EffectiveFn value;
    EffectiveFn provided;
    bool listed = true;
    bool declared = false;
  };
  EffectiveFn find(const std::string &name) const;

  Source source_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
};

// The process registry, reading the environment. A DLL has its own (each links the dxmt library).
Registry &processRegistry();

void provideEffective(const char *name, EffectiveFn fn, Registry &registry = processRegistry());
// The same, at namespace scope while the DLL loads: `sw::Effective kFooLine{"DXMT_FOO", fn};`
struct Effective {
  Effective(const char *name, EffectiveFn fn, Registry &registry = processRegistry()) {
    provideEffective(name, std::move(fn), registry);
  }
};

// One `NAME=value` of the startup line; no value: the registry's effective value for that name.
struct Field {
  std::string name;
  std::optional<std::string> value;
};

// The startup line: `fields` in their order, then the listed registry entries `fields` and `tail`
// don't name, sorted by name (byte order), then `tail` in its order. A field without a value that
// the registry has no entry for is left out.
std::string startupLine(const Registry &registry, const std::vector<Field> &fields, const std::vector<Field> &tail = {});

// How a value is written on the startup line: bools as 0/1, the rest as std::format's {} writes it.
template <class T>
std::string
formatValue(const T &v) {
  if constexpr (std::is_same_v<T, bool>)
    return v ? "1" : "0";
  else if constexpr (std::is_same_v<T, std::string>)
    return v;
  else
    return std::format("{}", v);
}

template <class T> using Parser = T (*)(const std::string &raw, const T &fallback);

// Whether the startup line shows the switch (Unlisted: a switch the line never had).
enum class Line : uint8_t { Listed, Unlisted };

template <class T> class Switch {
public:
  Switch(const char *name, T fallback, Parser<T> parser, Line line = Line::Listed,
         Registry &registry = processRegistry())
      : name_(name), fallback_(std::move(fallback)), parser_(parser), registry_(registry) {
    registry_.add(name_, [this] { return formatValue((*this)()); }, line == Line::Listed);
  }
  Switch(const Switch &) = delete;
  Switch &operator=(const Switch &) = delete;

  // The value parsed from the source now (one source read).
  T
  operator()() const {
    return parse(requested());
  }
  // The requested value: the source's raw text now ("" when unset).
  std::string
  requested() const {
    return registry_.requested(name_);
  }
  // `raw` parsed with this switch's grammar and fallback (for a caller that also needs the text
  // it parsed, from one read).
  T
  parse(const std::string &raw) const {
    return parser_(raw, fallback_);
  }
  const char *
  name() const {
    return name_;
  }

private:
  const char *name_;
  T fallback_;
  Parser<T> parser_;
  Registry &registry_;
};

// The grammars the fork's switches have always had, each one existing read exactly.
namespace legacy {

// `getEnvVar(...) != "0"`: on unless exactly "0" (the fallback, on, when unset or empty).
inline bool
notZero(const std::string &raw, const bool &fallback) {
  return raw.empty() ? fallback : raw != "0";
}

// `getEnvVar(...) == "1"`: on only for exactly "1" (the fallback, off, when unset or empty).
inline bool
exactlyOne(const std::string &raw, const bool &fallback) {
  return raw.empty() ? fallback : raw == "1";
}

// `strtoul(v, nullptr, 10)` as a uint32_t: leading space and sign accepted, trailing text ignored,
// "" -> the fallback. (Windows' unsigned long is 32 bits, so nothing is truncated there.)
inline uint32_t
strtoul32(const std::string &raw, const uint32_t &fallback) {
  return raw.empty() ? fallback : uint32_t(std::strtoul(raw.c_str(), nullptr, 10));
}

// Whether strtoul(raw) is one of `Allowed` (the full unsigned long, not truncated).
template <unsigned long... Allowed>
bool
ulongIsOneOf(const std::string &raw) {
  auto n = std::strtoul(raw.c_str(), nullptr, 10);
  return ((n == Allowed) || ...);
}

// strtoul(raw) if it is one of `Allowed`, else the fallback ("" too).
template <unsigned long... Allowed>
uint32_t
ulongOneOf(const std::string &raw, const uint32_t &fallback) {
  if (raw.empty() || !ulongIsOneOf<Allowed...>(raw))
    return fallback;
  return uint32_t(std::strtoul(raw.c_str(), nullptr, 10));
}

} // namespace legacy

} // namespace dxmt::sw
