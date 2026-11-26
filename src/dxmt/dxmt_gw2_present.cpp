#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_present_pace.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include "log/log.hpp"
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
// - aftergpu: presentedTime minus the frame's last GPU end that DXMT records, avg/max ms: the wait
//   for the vsync and the compositor. A chunk records its last command buffer's end: on Metal 3 with
//   the present split that is the present buffer's, on Metal 4 the chunk's last commit's (after a
//   present, the empty command buffer for timestamp queries unless DXMT_MTL4_TIMESTAMP_REUSE=1; with
//   DXMT_MTL4_PRESENT=mtl3, the Metal 3 present buffer, #227). The
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
// - gw2 (#224), at the end of the line: when the present pass ran against when its drawable was free.
//   The finish thread gives each frame's present pass its own GPU start and end (presentPassGpu: the
//   present buffer on Metal 3 and with DXMT_MTL4_PRESENT=mtl3, the pass's commit on Metal 4), joined
//   like aftergpu.
//   - drawwait: the pass's GPU start minus the present's encode time, avg/max ms, over shown frames.
//   - early: shown frames whose pass ended on the GPU before the surface's last frame left the
//     screen, that is before the frame after that one was presented (the layer's own clock). The
//     surface is the drawable texture's IOSurface id. A frame is left out when either time is
//     unknown and when its surface is seen for the first time. A gap in the frame numbers (a lost
//     record) starts every surface's history over.
//   - inuse: drawables whose surface IOSurfaceIsInUse called in use when the present was tracked.
// - gw2 (#242), last, on Metal 4 only: presentat, the drawable's present call minus its encode, avg/max
//   ms (presentAtSample). 0 on the Metal 4 order, which presents right after signalDrawable; with
//   DXMT_MTL4_PRESENT=done, when the present went out after the pass's commit completed.

namespace dxmt::gw2 {

namespace {

constexpr size_t kMaxPending = 1024;
// frames whose GPU end is kept for the join; at 240 fps that is over 8 s, four windows
constexpr size_t kMaxGpuFrames = 2048;

std::mutex g_mutex;
std::vector<WMTPresentedRecord> g_pending;
uint64_t g_lost = 0;

// a frame's GPU times (ns, 0: none): its last GPU end, and its present pass's start and end (#224)
struct GpuFrame {
  uint64_t end = 0;
  uint64_t pass_start = 0, pass_end = 0;
};

std::mutex g_gpu_mutex;
std::map<uint64_t, GpuFrame> g_gpu_end; // frame → its GPU times
uint64_t g_gpu_latest = 0;              // the newest frame with a GPU end: older frames are final

// gw2 (#224): a shown frame waiting for its GPU times: when the frame's surface was last replaced on
// screen before this frame (0: unknown)
struct Shown {
  uint64_t frame, presented_ns, encode_ns, replaced_ns;
};

// gw2 (#224): early's state across windows. Per surface the last frame shown from it and when the
// frame after it was presented (0: not yet); the previous record's frame, for gaps; the last shown
// frame and its surface, which the next shown frame replaces
struct SurfaceShown {
  uint64_t frame, replaced_ns;
};
constexpr size_t kMaxSurfaces = 64;
std::map<uint64_t, SurfaceShown> g_surfaces;
bool g_prev_valid = false, g_shown_valid = false;
uint64_t g_prev_frame = 0, g_shown_frame = 0, g_shown_surface = 0;

std::atomic<double> g_refresh_ms = 0;
std::atomic<uint32_t> g_vsyncs = 0;
std::atomic<double> g_limiter_ms = 0;

// section state across windows: the last frame that was shown, the lateness against the limiter's
// cadence, and the shown frames whose GPU end wasn't final yet
bool g_last_valid = false;
uint64_t g_last_frame = 0, g_last_presented_ns = 0;
double g_late_ms = 0, g_late_limiter_ms = 0;
std::vector<Shown> g_gpu_waiting;

std::string section(double, uint32_t);
SectionRegistrar g_registrar("PRESENT", section);

// gw2 (#224): one record, in frame order: updates early's state and returns when the record's
// surface last left the screen before it, 0 if unknown (a dropped frame, no surface, a surface seen
// the first time, a gap in the frame numbers)
uint64_t
surfaceStep(const WMTPresentedRecord &r) {
  if (!g_prev_valid || r.frame != g_prev_frame + 1) {
    // a record is missing: the last shown frame's successor is unknown, and the missing frame may have
    // shown on any surface, so every surface's history starts over
    g_shown_valid = false;
    g_surfaces.clear();
  }
  g_prev_valid = true;
  g_prev_frame = r.frame;
  if (!r.presented_ns)
    return 0; // never on screen: the last shown frame stays there
  // the last shown frame left the screen when this one reached it
  if (g_shown_valid) {
    auto it = g_surfaces.find(g_shown_surface);
    if (it != g_surfaces.end() && it->second.frame == g_shown_frame)
      it->second.replaced_ns = r.presented_ns;
  }
  uint64_t replaced_ns = 0;
  if (r.surface) {
    auto it = g_surfaces.find(r.surface);
    if (it != g_surfaces.end())
      replaced_ns = it->second.replaced_ns;
    if (it == g_surfaces.end() && g_surfaces.size() >= kMaxSurfaces)
      g_surfaces.clear(); // many swap chains or resizes: start over, each surface seen anew
    g_surfaces[r.surface] = {r.frame, 0};
  }
  g_shown_valid = r.surface != 0;
  g_shown_frame = r.frame;
  g_shown_surface = r.surface;
  return replaced_ns;
}

// gw2 (#157): DXMT_PRESENT_QUEUE (`default`, `two`, `pace`; anything else is `default`)
std::string
rawText(const std::string &raw, const std::string &fallback) {
  return raw.empty() ? fallback : raw;
}
sw::Switch<std::string> kPresentQueue{"DXMT_PRESENT_QUEUE", "default", rawText};
// the startup line shows the mode in effect: `unused` for pace without DXMT_STATS
sw::Effective kPresentQueueLine{"DXMT_PRESENT_QUEUE", [] {
  switch (presentQueue()) {
  case PresentQueue::Two:
    return std::string("two");
  case PresentQueue::Pace:
    return std::string(presentPaceOn() ? "pace" : "unused");
  default:
    return std::string("default");
  }
}};

// pace's signals: the encode thread writes them, the render thread reads them, once per present each
std::mutex g_pace_mutex;
LiveSkips g_live;
int64_t g_blocked_ns = 0;
uint64_t g_blocked_seq = 0;

// the PRESENTQ section: pace's sleeps in the window, and whether it has given up
std::atomic<uint64_t> g_pace_presents{0}, g_pace_sleeps{0}, g_pace_sleep_ns{0}, g_pace_sleep_max_ns{0};
std::atomic<bool> g_pace_gave_up{false};

std::string
paceSection(double, uint32_t) {
  if (!presentPaceOn())
    return {};
  uint64_t presents = g_pace_presents.exchange(0, std::memory_order_relaxed);
  uint64_t sleeps = g_pace_sleeps.exchange(0, std::memory_order_relaxed);
  uint64_t sum = g_pace_sleep_ns.exchange(0, std::memory_order_relaxed);
  uint64_t max_ns = g_pace_sleep_max_ns.exchange(0, std::memory_order_relaxed);
  return std::format("pace slept={}/{} sleep={:.2f}/{:.2f} gaveup={}", sleeps, presents,
                     sleeps ? sum / 1e6 / sleeps : 0.0, max_ns / 1e6, g_pace_gave_up.load(std::memory_order_relaxed) ? 1 : 0);
}
SectionRegistrar g_pace_registrar("PRESENTQ", paceSection);

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
  // gw2 (#242): the window's presentat
  uint64_t at_n = g_present_at.n.exchange(0, std::memory_order_relaxed);
  uint64_t at_sum = g_present_at.sum_ns.exchange(0, std::memory_order_relaxed);
  uint64_t at_max = g_present_at.max_ns.exchange(0, std::memory_order_relaxed);
  if (records.empty() && !lost && g_gpu_waiting.empty() && !at_n)
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
  std::vector<Shown> join; // last window's waiting first
  join.swap(g_gpu_waiting);
  size_t waited = join.size();
  uint32_t dropped = 0, inuse = 0;
  uint64_t missed = 0, starved = 0;
  for (auto &r : records) {
    inuse += r.in_use ? 1 : 0;             // gw2 (#224)
    uint64_t replaced_ns = surfaceStep(r); // gw2 (#224)
    if (!r.presented_ns) {
      dropped++;
      g_last_valid = false; // don't pair across a dropped frame
      continue;
    }
    latency.push_back(r.presented_ns > r.encode_ns ? (r.presented_ns - r.encode_ns) / 1e6 : 0.0);
    join.push_back({r.frame, r.presented_ns, r.encode_ns, replaced_ns});
    if (grid && g_last_valid && r.frame > g_last_frame && r.presented_ns > g_last_presented_ns) {
      double gap = (r.presented_ns - g_last_presented_ns) / 1e6;
      int64_t skipped;
      if (limiter_ms > 0) {
        // a quarter millisecond short of a refresh, against presentedTime's jitter
        g_late_ms = std::max(0.0, g_late_ms + gap - limiter_ms);
        skipped = (int64_t)std::floor((g_late_ms + 0.25) / refresh_ms);
        g_late_ms = std::max(0.0, g_late_ms - skipped * refresh_ms);
      } else {
        skipped = skippedRefreshes(gap, refresh_ms, vsyncs); // gw2 (#157): shared with LiveSkips
      }
      if (skipped > 0) {
        if (skipWasQueued(r.encode_ns, g_last_presented_ns))
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
  double drawwait = 0, drawwait_max = 0; // gw2 (#224)
  uint32_t drawwait_n = 0, early = 0;
  {
    std::lock_guard lock(g_gpu_mutex);
    uint64_t keep_from = 0; // GPU ends below this frame are no longer needed
    for (size_t i = 0; i < join.size(); i++) {
      auto &shown = join[i];
      uint64_t frame = shown.frame, presented_ns = shown.presented_ns;
      keep_from = std::max(keep_from, frame + 1);
      if (frame >= g_gpu_latest && i >= waited) {
        g_gpu_waiting.push_back(shown); // the finish thread isn't through it yet
        continue;
      }
      auto it = g_gpu_end.find(frame);
      // gw2 (#224): the present pass's times, once the frame is final
      if (it != g_gpu_end.end() && frame < g_gpu_latest) {
        auto &gpu = it->second;
        if (gpu.pass_start) {
          double v = gpu.pass_start > shown.encode_ns ? (gpu.pass_start - shown.encode_ns) / 1e6 : 0.0;
          drawwait += v;
          drawwait_max = std::max(drawwait_max, v);
          drawwait_n++;
        }
        if (gpu.pass_end && shown.replaced_ns && gpu.pass_end < shown.replaced_ns)
          early++;
      }
      // a GPU end of 0: none of the frame's command buffers reported one (GPUEndTime 0)
      if (it == g_gpu_end.end() || !it->second.end || frame >= g_gpu_latest || presented_ns <= it->second.end) {
        nogpu++;
        continue;
      }
      double v = (presented_ns - it->second.end) / 1e6;
      aftergpu += v;
      aftergpu_max = std::max(aftergpu_max, v);
      aftergpu_n++;
    }
    for (auto &w : g_gpu_waiting)
      keep_from = std::min(keep_from, w.frame);
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
  // gw2 (#224)
  if (drawwait_n)
    text += std::format(" drawwait={:.1f}/{:.1f}", drawwait / drawwait_n, drawwait_max);
  else
    text += " drawwait=-";
  text += std::format(" early={} inuse={}", early, inuse);
  if (at_n) // gw2 (#242)
    text += std::format(" presentat={:.2f}/{:.2f}", at_sum / 1e6 / at_n, at_max / 1e6);
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
  if (presentPaceOn() && n) {
    // gw2 (#157): pace's per-frame skip count, as the section counts them, in frame order. An insertion
    // sort: mingw-GCC's -Warray-bounds misreads std::sort on this 8-record array
    uint32_t m = std::min<uint32_t>(n, WMT_PRESENTED_BATCH);
    for (uint32_t i = 1; i < m; i++)
      for (uint32_t j = i; j > 0 && out[j].frame < out[j - 1].frame; j--)
        std::swap(out[j], out[j - 1]);
    double refresh_ms = g_refresh_ms.load(std::memory_order_relaxed);
    uint32_t vsyncs = g_vsyncs.load(std::memory_order_relaxed);
    double limiter_ms = g_limiter_ms.load(std::memory_order_relaxed);
    std::lock_guard lock(g_pace_mutex);
    for (uint32_t i = 0; i < m; i++)
      g_live.add(out[i].frame, out[i].encode_ns, out[i].presented_ns, refresh_ms, vsyncs, limiter_ms);
  }
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
  auto &end = g_gpu_end[frame].end;
  end = std::max(end, gpu_end_ns);
  g_gpu_latest = std::max(g_gpu_latest, frame);
  // nothing collects them while no records come (the section would count those frames as nogpu)
  while (g_gpu_end.size() > kMaxGpuFrames)
    g_gpu_end.erase(g_gpu_end.begin());
}

void
presentPassGpu(uint64_t frame, uint64_t gpu_start_ns, uint64_t gpu_end_ns) {
  std::lock_guard lock(g_gpu_mutex);
  auto &gpu = g_gpu_end[frame];
  gpu.pass_start = gpu_start_ns;
  gpu.pass_end = gpu_end_ns;
  while (g_gpu_end.size() > kMaxGpuFrames)
    g_gpu_end.erase(g_gpu_end.begin());
}

void
presentSetPacing(double refresh_ms, uint32_t vsyncs, double limiter_ms) {
  g_refresh_ms.store(refresh_ms, std::memory_order_relaxed);
  g_vsyncs.store(vsyncs, std::memory_order_relaxed);
  g_limiter_ms.store(limiter_ms, std::memory_order_relaxed);
}

PresentQueue
presentQueue() {
  static const PresentQueue mode = [] {
    auto v = kPresentQueue();
    return v == "two" ? PresentQueue::Two : v == "pace" ? PresentQueue::Pace : PresentQueue::Default;
  }();
  return mode;
}

bool
presentPaceOn() {
  static const bool on = [] {
    bool asked = presentQueue() == PresentQueue::Pace;
    if (asked && !statsEnabled())
      Logger::warn("Present queue: DXMT_PRESENT_QUEUE=pace needs DXMT_STATS=1 (its presented records and "
                   "nextDrawable timer); pace does nothing");
    return asked && statsEnabled();
  }();
  return on;
}

void
presentPaceBlocked(int64_t ns, uint64_t seq) {
  std::lock_guard lock(g_pace_mutex);
  g_blocked_ns = ns;
  g_blocked_seq = seq;
}

PaceSignals
presentPaceSignals() {
  std::lock_guard lock(g_pace_mutex);
  return {g_blocked_ns, g_blocked_seq, g_live.missed(), g_live.starved()};
}

void
presentPaceStep(int64_t ns, bool gave_up) {
  g_pace_presents.fetch_add(1, std::memory_order_relaxed);
  if (gave_up)
    g_pace_gave_up.store(true, std::memory_order_relaxed);
  if (ns <= 0)
    return;
  g_pace_sleeps.fetch_add(1, std::memory_order_relaxed);
  g_pace_sleep_ns.fetch_add(uint64_t(ns), std::memory_order_relaxed);
  if (uint64_t(ns) > g_pace_sleep_max_ns.load(std::memory_order_relaxed))
    g_pace_sleep_max_ns.store(uint64_t(ns), std::memory_order_relaxed);
}

} // namespace dxmt::gw2
