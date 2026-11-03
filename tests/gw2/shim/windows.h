#pragma once
// gw2 (#28): a host-side stand-in for the Win32 calls dxmt_gw2_stats.cpp makes, with simulated
// threads: fake::tid is the "current thread", fake::exitThread() runs its fiber-local callbacks the
// way the loader does when a thread exits.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>

typedef unsigned long DWORD;
typedef void *PVOID;
typedef int BOOL;
#define WINAPI
#define FLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
typedef void (*PFLS_CALLBACK_FUNCTION)(PVOID);

namespace fake {
inline DWORD tid = 1;
inline std::map<DWORD, std::map<DWORD, void *>> fls; // per thread: slot -> value
inline std::map<DWORD, PFLS_CALLBACK_FUNCTION> slots;
inline DWORD next_slot = 1;
inline void
exitThread() {
  for (auto &[slot, value] : fls[tid])
    if (value && slots.count(slot))
      slots[slot](value);
  fls.erase(tid);
}
} // namespace fake

inline DWORD
GetCurrentThreadId() {
  return fake::tid;
}
inline DWORD
FlsAlloc(PFLS_CALLBACK_FUNCTION cb) {
  DWORD i = fake::next_slot++;
  fake::slots[i] = cb;
  return i;
}
inline PVOID
FlsGetValue(DWORD i) {
  auto &m = fake::fls[fake::tid];
  auto it = m.find(i);
  return it == m.end() ? nullptr : it->second;
}
inline BOOL
FlsSetValue(DWORD i, PVOID v) {
  fake::fls[fake::tid][i] = v;
  return 1;
}
inline BOOL
FlsFree(DWORD i) {
  auto cb = fake::slots[i];
  for (auto &[t, m] : fake::fls) {
    auto it = m.find(i);
    if (it != m.end() && it->second) {
      cb(it->second);
      m.erase(it);
    }
  }
  fake::slots.erase(i);
  return 1;
}
inline char *
GetEnvironmentStringsA() {
  return nullptr;
}
inline BOOL
FreeEnvironmentStringsA(char *) {
  return 1;
}
