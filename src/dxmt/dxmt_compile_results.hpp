#pragma once

// gw2 (C4, architecture council §1): shared results by stable key. One owner per key: the first job
// that claims a key builds it and publishes the result (or abandons the claim when it can't build
// it); a job that finds the key claimed by another parks on that owner, and one that finds it done
// takes the result. Warming (W2/W3) and live jobs share the table, so each key is built once per
// process, whoever comes first.
//
// The owner is a running job: claims are taken in a job's run and published or abandoned before that
// run ends, so a job parked on an owner is always woken (by the owner's completion), and then finds
// the key done, or free to claim. Header-only standard C++ (tests/gw2/warm_test.cpp).

#include "thread.hpp"
#include <mutex>
#include <unordered_map>

namespace dxmt::compile {

template <typename Key, typename Value, typename Owner, typename Hash = std::hash<Key>> class ResultTable {
public:
  enum class Claim { Mine, Done, Busy };

  // Mine: `self` owns the key now (or already did); build, then publish or abandon. Done: `*value`
  // is the result. Busy: `*owner` builds it; park on it.
  Claim
  claim(const Key &key, Owner self, Value *value, Owner *owner) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    auto [it, inserted] = entries_.try_emplace(key);
    auto &e = it->second;
    if (inserted) {
      e.owner = self;
      return Claim::Mine;
    }
    if (e.done) {
      *value = e.value;
      return Claim::Done;
    }
    if (e.owner == self)
      return Claim::Mine;
    *owner = e.owner;
    return Claim::Busy;
  }

  // Without claiming: Done (value), Busy (owner), or Mine meaning "absent" (nobody has it).
  Claim
  find(const Key &key, Value *value, Owner *owner) const {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
      return Claim::Mine;
    if (it->second.done) {
      *value = it->second.value;
      return Claim::Done;
    }
    *owner = it->second.owner;
    return Claim::Busy;
  }

  // The owner's result: the key is done for good.
  void
  publish(const Key &key, Owner self, Value value) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.done || it->second.owner != self)
      return; // not ours (never happens: claims are kept until published or abandoned)
    it->second.done = true;
    it->second.value = std::move(value);
    it->second.owner = Owner{};
  }

  // The owner couldn't build it: the key is free again (the next claim builds it its own way).
  void
  abandon(const Key &key, Owner self) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it != entries_.end() && !it->second.done && it->second.owner == self)
      entries_.erase(it);
  }

  size_t
  size() const {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return entries_.size();
  }

private:
  struct Entry {
    Owner owner{};
    bool done = false;
    Value value{};
  };
  mutable dxmt::mutex mutex_;
  std::unordered_map<Key, Entry, Hash> entries_;
};

} // namespace dxmt::compile
