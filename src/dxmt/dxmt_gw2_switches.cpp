#include "dxmt_gw2_switches.hpp"
#include "util_env.hpp"
#include <set>

namespace dxmt::sw {

Registry::Registry(Source source) : source_(std::move(source)) {}

std::string
Registry::requested(const char *name) const {
  return source_(name);
}

bool
Registry::add(const char *name, EffectiveFn value, bool listed) {
  std::lock_guard lock(mutex_);
  auto &e = entries_[name];
  if (e.declared)
    return false;
  e.value = std::move(value);
  e.listed = listed;
  e.declared = true;
  return true;
}

void
Registry::provideEffective(const char *name, EffectiveFn fn) {
  std::lock_guard lock(mutex_);
  entries_[name].provided = std::move(fn);
}

EffectiveFn
Registry::find(const std::string &name) const {
  std::lock_guard lock(mutex_);
  auto it = entries_.find(name);
  if (it == entries_.end() || !it->second.listed)
    return {};
  return it->second.provided ? it->second.provided : it->second.value;
}

std::optional<std::string>
Registry::effective(const std::string &name) const {
  // called unlocked: a formatter reads its switch's source
  auto fn = find(name);
  if (!fn)
    return std::nullopt;
  return fn();
}

std::vector<std::string>
Registry::listed() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> names;
  for (auto &[name, e] : entries_)
    if (e.listed && (e.provided || e.value))
      names.push_back(name);
  return names;
}

Registry &
processRegistry() {
  static Registry registry([](const char *name) { return env::getEnvVar(name); });
  return registry;
}

void
provideEffective(const char *name, EffectiveFn fn, Registry &registry) {
  registry.provideEffective(name, std::move(fn));
}

std::string
startupLine(const Registry &registry, const std::vector<Field> &fields, const std::vector<Field> &tail) {
  std::string line;
  auto put = [&](const std::string &name, const std::string &value) {
    if (!line.empty())
      line += ' ';
    line += name;
    line += '=';
    line += value;
  };
  auto putField = [&](const Field &f) {
    if (f.value)
      put(f.name, *f.value);
    else if (auto v = registry.effective(f.name))
      put(f.name, *v);
  };
  std::set<std::string> named;
  for (auto &f : fields)
    named.insert(f.name);
  for (auto &f : tail)
    named.insert(f.name);

  for (auto &f : fields)
    putField(f);
  for (auto &name : registry.listed()) // sorted: the registry's map
    if (!named.contains(name))
      if (auto v = registry.effective(name))
        put(name, *v);
  for (auto &f : tail)
    putField(f);
  return line;
}

} // namespace dxmt::sw
