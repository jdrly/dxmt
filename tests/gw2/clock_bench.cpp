// gw2 (#28): which clock the PACE/MAP scopes should read (gw2::nowNs). A Windows .exe, built with the
// fork's own toolchain and run headless under the engine by tests/gw2/clock_bench.zsh (not part of the
// DLL build, not a tests/gw2/run.zsh test: it needs Wine).
//
//   x86_64-w64-mingw32-g++ -O2 -std=c++20 -static tests/gw2/clock_bench.cpp -o clock_bench.exe
//   clock_bench.exe [calls]
//
// For each candidate: ns per call (a tight loop of `calls` reads, best of 5, timed against QPC),
// the smallest and the median nonzero step between consecutive reads, the share of reads equal to
// the one before (a read that didn't advance), and how many stepped backwards. Last, the interrupt
// time in KUSER_SHARED_DATA (a plain memory read): how often its value changes. Everything is read
// through portable code (no rdtsc, no intrinsics).

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <time.h>
#include <vector>

namespace {

LARGE_INTEGER g_freq;

int64_t
qpcNs() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  int64_t f = g_freq.QuadPart;
  return c.QuadPart / f * 1'000'000'000 + c.QuadPart % f * 1'000'000'000 / f;
}

int64_t
steadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t
preciseFtNs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  return int64_t((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) * 100;
}

int64_t
monotonicNs() { // winpthreads' clock_gettime
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

typedef LONG(NTAPI *NtQpcFn)(LARGE_INTEGER *, LARGE_INTEGER *);
NtQpcFn g_ntqpc;

int64_t
ntQpcNs() { // ntdll directly, skipping kernel32/kernelbase's two thunks
  LARGE_INTEGER c;
  g_ntqpc(&c, nullptr);
  return c.QuadPart * 100;
}

// KUSER_SHARED_DATA (0x7ffe0000): InterruptTime at 0x08, the KSYSTEM_TIME {LowPart, High1Time,
// High2Time}, read as Windows does (High1, Low, until High2 == High1). In 100 ns units.
int64_t
kuserInterruptNs() {
  volatile ULONG *low = reinterpret_cast<volatile ULONG *>(0x7ffe0008);
  volatile LONG *high1 = reinterpret_cast<volatile LONG *>(0x7ffe000c);
  volatile LONG *high2 = reinterpret_cast<volatile LONG *>(0x7ffe0010);
  LONG h;
  ULONG l;
  do {
    h = *high1;
    l = *low;
  } while (h != *high2);
  return ((int64_t(h) << 32) | l) * 100;
}

typedef void(WINAPI *QitFn)(ULONGLONG *);
QitFn g_qit;

int64_t
queryInterruptNs() {
  ULONGLONG t;
  g_qit(&t);
  return int64_t(t) * 100;
}

int64_t
tick64Ns() {
  return int64_t(GetTickCount64()) * 1'000'000;
}

struct Clock {
  const char *name;
  int64_t (*fn)();
};

double
nsPerCall(int64_t (*fn)(), int calls) {
  double best = 1e30;
  for (int r = 0; r < 5; r++) {
    volatile int64_t sink = 0;
    int64_t t0 = qpcNs();
    for (int i = 0; i < calls; i++)
      sink = sink + fn();
    int64_t t1 = qpcNs();
    best = std::min(best, double(t1 - t0) / calls);
  }
  return best;
}

void
steps(int64_t (*fn)(), int reads, int64_t &min_step, int64_t &median_step, double &same, int &back) {
  std::vector<int64_t> v(reads);
  for (int i = 0; i < reads; i++)
    v[i] = fn();
  std::vector<int64_t> d;
  int eq = 0;
  back = 0;
  for (int i = 1; i < reads; i++) {
    int64_t s = v[i] - v[i - 1];
    if (s > 0)
      d.push_back(s);
    else if (s == 0)
      eq++;
    else
      back++;
  }
  same = double(eq) / (reads - 1);
  if (d.empty()) {
    min_step = median_step = 0;
    return;
  }
  std::sort(d.begin(), d.end());
  min_step = d.front();
  median_step = d[d.size() / 2];
}

// How often a memory-mapped clock changes: spin for `ms` (timed with QPC), count distinct values.
void
updates(int64_t (*fn)(), int ms) {
  int64_t end = qpcNs() + int64_t(ms) * 1'000'000;
  int64_t last = fn(), last_at = qpcNs(), max_gap = 0, changes = 0;
  while (true) {
    int64_t now = qpcNs();
    if (now >= end)
      break;
    int64_t v = fn();
    if (v != last) {
      changes++;
      max_gap = std::max(max_gap, now - last_at);
      last = v;
      last_at = now;
    }
  }
  max_gap = std::max(max_gap, end - last_at);
  std::printf("  spun %d ms: %lld changes, longest unchanged %.3f ms\n", ms, (long long)changes, max_gap / 1e6);
}

} // namespace

int
main(int argc, char **argv) {
  int calls = argc > 1 ? std::atoi(argv[1]) : 1'000'000;
  QueryPerformanceFrequency(&g_freq);
  g_ntqpc = reinterpret_cast<NtQpcFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryPerformanceCounter"));
  g_qit = reinterpret_cast<QitFn>(GetProcAddress(GetModuleHandleA("kernelbase.dll"), "QueryInterruptTime"));
  std::printf("QPC frequency %lld Hz; %d calls a loop\n", (long long)g_freq.QuadPart, calls);

  std::vector<Clock> clocks = {
      {"QueryPerformanceCounter", qpcNs},
      {"NtQueryPerformanceCounter", ntQpcNs},
      {"steady_clock::now", steadyNs},
      {"GetSystemTimePreciseAsFileTime", preciseFtNs},
      {"clock_gettime(MONOTONIC)", monotonicNs},
      {"KUSER InterruptTime", kuserInterruptNs},
      {"GetTickCount64", tick64Ns},
  };
  if (g_qit)
    clocks.push_back({"QueryInterruptTime", queryInterruptNs});

  std::printf("%-32s %10s %12s %12s %8s %6s\n", "clock", "ns/call", "min step ns", "median ns", "same", "back");
  for (auto &c : clocks) {
    nsPerCall(c.fn, calls / 10); // warm up
    double ns = nsPerCall(c.fn, calls);
    int64_t mn, med;
    double same;
    int back;
    steps(c.fn, 200'000, mn, med, same, back);
    std::printf(
        "%-32s %10.1f %12lld %12lld %7.1f%% %6d\n", c.name, ns, (long long)mn, (long long)med, same * 100, back
    );
  }
  std::printf("KUSER InterruptTime, idle process:\n");
  updates(kuserInterruptNs, 1000);
  return 0;
}
