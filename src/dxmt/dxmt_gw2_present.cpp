#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_stats.hpp"
#include "winemetal.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// gw2 (#7): the PRESENT section.
//
// - presented: the drawable's presentedTime minus the time its present was encoded (the unix side
//   takes CACurrentMediaTime() right before presentDrawable*), avg/p95/max ms.
// - aftergpu: presentedTime minus the frame's last GPU end that DXMT records (the split present
//   buffer's copy isn't among them), avg/max ms: the wait for the vsync and the compositor. The
//   GPU ends are kept per frame until the frame's record is joined; a record whose frame the finish
//   thread hasn't finished yet waits for the next window, once. nogpu: shown frames without a
//   usable GPU end (none recorded, or still unfinished a window later).
// - missed / starved: refreshes skipped between two consecutive presented frames beyond the ones
//   the pacing asks for (presentSetPacing). missed: the later frame was already encoded when the
//   earlier one reached the screen (it was queued and still didn't make the next vsync); starved:
//   it wasn't (the game or the encode thread was late, or the deadline lay past the next vsync).
//   With the frame limiter (a cap that isn't a whole number of refreshes) the frames alternate
//   between gaps of one and two refreshes, so the gaps are counted against the limiter's cadence
//   instead: their lateness against the limiter interval adds up (never below 0), and every whole
//   refresh it reaches is a skipped one.
// - dropped: drawables whose presentedTime is 0 (never shown).

namespace dxmt::gw2 {

namespace {

constexpr size_t kMaxPending = 1024;
// frames whose GPU end is kept for the join; at 240 fps that is over 8 s, four windows
constexpr size_t kMaxGpuFrames = 2048;

std::mutex g_mutex;
std::vector<WMTPresentedRecord> g_pending;
uint64_t g_lost = 0;

std::mutex g_gpu_mutex;
std::map<uint64_t, uint64_t> g_gpu_end; // frame → its last GPU end (ns)
uint64_t g_gpu_latest = 0;              // the newest frame with a GPU end: older frames are final

std::atomic<double> g_refresh_ms = 0;
std::atomic<uint32_t> g_vsyncs = 0;
std::atomic<double> g_limiter_ms = 0;

// section state across windows: the last frame that was shown, the lateness against the limiter's
// cadence, and the shown frames whose GPU end wasn't final yet (frame, presented_ns)
bool g_last_valid = false;
uint64_t g_last_frame = 0, g_last_presented_ns = 0;
double g_late_ms = 0, g_late_limiter_ms = 0;
std::vector<std::pair<uint64_t, uint64_t>> g_gpu_waiting;

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
  if (records.empty() && !lost && g_gpu_waiting.empty())
    return {};
  std::sort(records.begin(), records.end(), [](auto &a, auto &b) { return a.frame < b.frame; });

  double refresh_ms = g_refresh_ms.load(std::memory_order_relaxed);
  uint32_t vsyncs = g_vsyncs.load(std::memory_order_relaxed);
  double limiter_ms = g_limiter_ms.load(std::memory_order_relaxed);
  bool grid = refresh_ms > 0 && vsyncs > 0;
  if (limiter_ms != g_late_limiter_ms) {
    g_late_limiter_ms = limiter_ms;
    g_late_ms = 0;
  }

  std::vector<double> latency;
  latency.reserve(records.size());
  std::vector<std::pair<uint64_t, uint64_t>> join; // (frame, presented_ns): last window's waiting first
  join.swap(g_gpu_waiting);
  size_t waited = join.size();
  uint32_t dropped = 0;
  uint64_t missed = 0, starved = 0;
  for (auto &r : records) {
    if (!r.presented_ns) {
      dropped++;
      g_last_valid = false; // don't pair across a dropped frame
      continue;
    }
    latency.push_back(r.presented_ns > r.encode_ns ? (r.presented_ns - r.encode_ns) / 1e6 : 0.0);
    join.emplace_back(r.frame, r.presented_ns);
    if (grid && g_last_valid && r.frame > g_last_frame && r.presented_ns > g_last_presented_ns) {
      double gap = (r.presented_ns - g_last_presented_ns) / 1e6;
      int64_t skipped;
      if (limiter_ms > 0) {
        // a quarter millisecond short of a refresh, against presentedTime's jitter
        g_late_ms = std::max(0.0, g_late_ms + gap - limiter_ms);
        skipped = (int64_t)std::floor((g_late_ms + 0.25) / refresh_ms);
        g_late_ms = std::max(0.0, g_late_ms - skipped * refresh_ms);
      } else {
        skipped = std::max<int64_t>(0, std::llround(gap / refresh_ms) - (int64_t)vsyncs);
      }
      if (skipped > 0) {
        if (r.encode_ns < g_last_presented_ns)
          missed += skipped;
        else
          starved += skipped;
      }
    } else {
      g_late_ms = 0;
    }
    g_last_valid = true;
    g_last_frame = r.frame;
    g_last_presented_ns = r.presented_ns;
  }

  double aftergpu = 0, aftergpu_max = 0;
  uint32_t aftergpu_n = 0, nogpu = 0;
  {
    std::lock_guard lock(g_gpu_mutex);
    uint64_t keep_from = 0; // GPU ends below this frame are no longer needed
    for (size_t i = 0; i < join.size(); i++) {
      auto [frame, presented_ns] = join[i];
      keep_from = std::max(keep_from, frame + 1);
      if (frame >= g_gpu_latest && i >= waited) {
        g_gpu_waiting.emplace_back(frame, presented_ns); // the finish thread isn't through it yet
        continue;
      }
      auto it = g_gpu_end.find(frame);
      if (it == g_gpu_end.end() || frame >= g_gpu_latest || presented_ns <= it->second) {
        nogpu++;
        continue;
      }
      double v = (presented_ns - it->second) / 1e6;
      aftergpu += v;
      aftergpu_max = std::max(aftergpu_max, v);
      aftergpu_n++;
    }
    for (auto &w : g_gpu_waiting)
      keep_from = std::min(keep_from, w.first);
    g_gpu_end.erase(g_gpu_end.begin(), g_gpu_end.lower_bound(keep_from));
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
  text += std::format("nogpu={} ", nogpu);
  if (grid && limiter_ms > 0)
    text += std::format("missed={} starved={} target={:.2f}x{:.2f} ", missed, starved, limiter_ms / refresh_ms,
                        refresh_ms);
  else if (grid)
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
  auto &end = g_gpu_end[frame];
  end = std::max(end, gpu_end_ns);
  g_gpu_latest = std::max(g_gpu_latest, frame);
  // nothing collects them while no records come (the section would count those frames as nogpu)
  while (g_gpu_end.size() > kMaxGpuFrames)
    g_gpu_end.erase(g_gpu_end.begin());
}

void
presentSetPacing(double refresh_ms, uint32_t vsyncs, double limiter_ms) {
  g_refresh_ms.store(refresh_ms, std::memory_order_relaxed);
  g_vsyncs.store(vsyncs, std::memory_order_relaxed);
  g_limiter_ms.store(limiter_ms, std::memory_order_relaxed);
}

} // namespace dxmt::gw2
