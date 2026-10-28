#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_stats.hpp"
#include "winemetal.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <vector>

// gw2 (#7): the PRESENT section.
//
// - presented: the drawable's presentedTime minus the time its present was encoded (the unix side
//   takes CACurrentMediaTime() right before presentDrawable*), avg/p95/max ms.
// - aftergpu: presentedTime minus the frame's last GPU end that DXMT records (the split present
//   buffer's copy isn't among them), avg/max ms: the wait for the vsync and the compositor.
// - missed / starved: refreshes skipped between two consecutive presented frames beyond the ones
//   the pacing asks for (presentSetPacing). missed: the later frame was already encoded when the
//   earlier one reached the screen (it was queued and still didn't make the next vsync); starved:
//   it wasn't (the game or the encode thread was late, or the deadline lay past the next vsync).
// - dropped: drawables whose presentedTime is 0 (never shown).

namespace dxmt::gw2 {

namespace {

constexpr size_t kMaxPending = 1024;
constexpr uint64_t kGpuRing = 64;

struct GpuEnd {
  uint64_t frame = ~0ull;
  uint64_t end_ns = 0;
};

std::mutex g_mutex;
std::vector<WMTPresentedRecord> g_pending;
uint64_t g_lost = 0;

std::mutex g_gpu_mutex;
GpuEnd g_gpu[kGpuRing];

std::atomic<double> g_refresh_ms = 0;
std::atomic<uint32_t> g_vsyncs = 0;

// section state across windows: the last frame that was shown
bool g_last_valid = false;
uint64_t g_last_frame = 0, g_last_presented_ns = 0;

std::string section(double, uint32_t);
SectionRegistrar g_registrar("PRESENT", section);

std::string
section(double, uint32_t) {
  std::vector<WMTPresentedRecord> records;
  uint64_t lost;
  {
    std::lock_guard lock(g_mutex);
    records.swap(g_pending);
    lost = g_lost;
    g_lost = 0;
  }
  if (records.empty() && !lost)
    return {};
  std::sort(records.begin(), records.end(), [](auto &a, auto &b) { return a.frame < b.frame; });

  double refresh_ms = g_refresh_ms.load(std::memory_order_relaxed);
  uint32_t vsyncs = g_vsyncs.load(std::memory_order_relaxed);
  bool grid = refresh_ms > 0 && vsyncs > 0;

  std::vector<double> latency;
  latency.reserve(records.size());
  double aftergpu = 0, aftergpu_max = 0;
  uint32_t aftergpu_n = 0, dropped = 0;
  uint64_t missed = 0, starved = 0;
  for (auto &r : records) {
    if (!r.presented_ns) {
      dropped++;
      g_last_valid = false; // don't pair across a dropped frame
      continue;
    }
    latency.push_back(r.presented_ns > r.encode_ns ? (r.presented_ns - r.encode_ns) / 1e6 : 0.0);
    {
      std::lock_guard lock(g_gpu_mutex);
      auto &g = g_gpu[r.frame % kGpuRing];
      if (g.frame == r.frame && g.end_ns && r.presented_ns > g.end_ns) {
        double v = (r.presented_ns - g.end_ns) / 1e6;
        aftergpu += v;
        aftergpu_max = std::max(aftergpu_max, v);
        aftergpu_n++;
      }
    }
    if (grid && g_last_valid && r.frame > g_last_frame && r.presented_ns > g_last_presented_ns) {
      double gap = (r.presented_ns - g_last_presented_ns) / 1e6;
      auto refreshes = (int64_t)std::llround(gap / refresh_ms);
      if (refreshes > (int64_t)vsyncs) {
        if (r.encode_ns < g_last_presented_ns)
          missed += refreshes - vsyncs;
        else
          starved += refreshes - vsyncs;
      }
    }
    g_last_valid = true;
    g_last_frame = r.frame;
    g_last_presented_ns = r.presented_ns;
  }

  std::string text;
  if (!latency.empty()) {
    double sum = 0;
    for (double v : latency)
      sum += v;
    std::sort(latency.begin(), latency.end());
    double p95 = latency[std::min(latency.size() - 1, size_t(0.95 * (latency.size() - 1) + 0.5))];
    text += std::format("presented={:.1f}/{:.1f}/{:.1f} ", sum / latency.size(), p95, latency.back());
  } else {
    text += "presented=- ";
  }
  if (aftergpu_n)
    text += std::format("aftergpu={:.1f}/{:.1f} ", aftergpu / aftergpu_n, aftergpu_max);
  else
    text += "aftergpu=- ";
  if (grid)
    text += std::format("missed={} starved={} target={}x{:.2f} ", missed, starved, vsyncs, refresh_ms);
  else
    text += "missed=- starved=- target=- ";
  text += std::format("n={} dropped={} lost={}", records.size(), dropped, lost);
  return text;
}

} // namespace

void
presentTrack(uint64_t drawable, uint64_t frame) {
  WMTPresentedRecord out[WMT_PRESENTED_BATCH];
  uint32_t lost = 0;
  uint32_t n = MTLDrawable_trackPresented(drawable, frame, out, WMT_PRESENTED_BATCH, &lost);
  if (!n && !lost)
    return;
  std::lock_guard lock(g_mutex);
  for (uint32_t i = 0; i < n; i++) {
    if (g_pending.size() < kMaxPending)
      g_pending.push_back(out[i]);
    else
      g_lost++;
  }
  g_lost += lost;
}

void
presentGpuEnd(uint64_t frame, uint64_t gpu_end_ns) {
  std::lock_guard lock(g_gpu_mutex);
  auto &g = g_gpu[frame % kGpuRing];
  if (g.frame != frame) {
    g.frame = frame;
    g.end_ns = 0;
  }
  g.end_ns = std::max(g.end_ns, gpu_end_ns);
}

void
presentSetPacing(double refresh_ms, uint32_t vsyncs) {
  g_refresh_ms.store(refresh_ms, std::memory_order_relaxed);
  g_vsyncs.store(vsyncs, std::memory_order_relaxed);
}

} // namespace dxmt::gw2
