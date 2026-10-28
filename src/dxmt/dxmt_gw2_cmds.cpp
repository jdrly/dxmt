#include "dxmt_gw2_cmds.hpp"
#include "dxmt_gw2_stats.hpp"
#include <atomic>
#include <format>
#include <string>

namespace dxmt::gw2 {

const char *const kCommitWhyNames[kCommitWhyCount] = {
    "none",     "other", "flush",   "getdata", "map",   "occlusion",  "timestamp", "event",    "staging", "readback",
    "cmdlist",  "present", "fence", "keyedmutex", "idle", "poll", "timer", "watchdog",
};
const char *const kPassCloseNames[kPassCloseCount] = {
    "other", "render", "compute", "blit",  "targets", "clear",   "resolve",
    "query", "flush",  "present", "state", "upscale", "copy",
};
const char *const kPromoteNames[kPromoteCount] = {
    "skip", "reached", "nodefer", "gpuidle", "present", "explicit", "poll", "timer", "watchdog",
};
const char *const kEncNames[kEncCount] = {
    "render", "compute", "blit", "clear", "resolve", "present", "upscale", "event", "timestamp",
};
const char *const kRefuseNames[kRefuseCount] = {
    "none", "target", "argbuf", "load", "store", "vfence", "dep", "sync",
};

namespace {

using counter = std::atomic<uint64_t>;

counter g_commit[kCommitWhyCount];
counter g_pass_close[kPassCloseCount];
counter g_promote[kPromoteCount];
std::atomic<uint32_t> g_promote_min{0};

counter g_draws, g_dispatches, g_tile, g_pso, g_use_resource, g_blit_cmds;
counter g_enc[kEncCount];
counter g_refuse[kRefuseCount];
counter g_load[kAspectCount], g_store[kAspectCount];
counter g_chunks, g_chunk_encoders;
std::atomic<uint32_t> g_chunk_encoders_max{0};

inline void
add(counter &total, uint64_t value) {
  if (value)
    total.fetch_add(value, std::memory_order_relaxed);
}

inline uint64_t
take(counter &total) {
  return total.exchange(0, std::memory_order_relaxed);
}

// "a:1.0,b:2.5": per frame, entries that are zero left out unless always_shown says otherwise.
template <unsigned N>
std::string
perFrameList(counter (&totals)[N], const char *const (&names)[N], double frames, unsigned first, int always_shown) {
  std::string out;
  for (unsigned i = first; i < N; i++) {
    uint64_t v = take(totals[i]);
    if (!v && (int)i != always_shown)
      continue;
    if (!out.empty())
      out += ',';
    out += std::format("{}:{:.1f}", names[i], v / frames);
  }
  return out.empty() ? "none" : out;
}

std::string
cmdsSection(double, uint32_t frames) {
  if (!frames)
    return {};
  double f = frames;
  uint64_t commits = 0;
  for (unsigned i = 0; i < kCommitWhyCount; i++)
    commits += g_commit[i].load(std::memory_order_relaxed);
  std::string out = std::format("cb={:.1f}", commits / f);
  out += " cbwhy=" + perFrameList(g_commit, kCommitWhyNames, f, (unsigned)CommitWhy::Other, (int)CommitWhy::Other);
  out += " close=" + perFrameList(g_pass_close, kPassCloseNames, f, 0, -1);
  uint32_t min = g_promote_min.load(std::memory_order_relaxed);
  if (!min) {
    for (auto &c : g_promote)
      take(c);
    out += " promote=off";
  } else {
    // window totals: the forced ends (poll, timer, watchdog) are rare
    out += std::format(" promote=min:{}", min);
    for (unsigned i = 0; i < kPromoteCount; i++)
      out += std::format(",{}:{}", kPromoteNames[i], take(g_promote[i]));
  }
  return out;
}

std::string
encodeSection(double, uint32_t frames) {
  if (!frames)
    return {};
  double f = frames;
  std::string out = std::format(
      "draws={:.1f} dispatches={:.1f} tile={:.1f} pso={:.1f} useres={:.1f} blitcmds={:.1f}", take(g_draws) / f,
      take(g_dispatches) / f, take(g_tile) / f, take(g_pso) / f, take(g_use_resource) / f, take(g_blit_cmds) / f
  );
  out += " enc=" + perFrameList(g_enc, kEncNames, f, 0, -1);
  uint64_t chunks = take(g_chunks);
  uint64_t chunk_encoders = take(g_chunk_encoders);
  uint32_t chunk_max = g_chunk_encoders_max.exchange(0, std::memory_order_relaxed);
  out += std::format(" perchunk={:.1f}/{}", chunks ? double(chunk_encoders) / chunks : 0.0, chunk_max);
  auto bytes = [&](counter (&totals)[kAspectCount]) {
    uint64_t c = take(totals[0]), d = take(totals[1]), s = take(totals[2]);
    return std::format("color:{:.1f},depth:{:.1f},stencil:{:.1f}", c / 1e6 / f, d / 1e6 / f, s / 1e6 / f);
  };
  out += " loadMB=" + bytes(g_load);
  out += " storeMB=" + bytes(g_store);
  out += " refuse=";
  for (unsigned i = (unsigned)Refuse::Target; i < kRefuseCount; i++)
    out += std::format("{}{}:{:.1f}", i == (unsigned)Refuse::Target ? "" : ",", kRefuseNames[i], take(g_refuse[i]) / f);
  return out;
}

SectionRegistrar s_cmds("CMDS", &cmdsSection);
SectionRegistrar s_encode("ENCODE", &encodeSection);

} // namespace

void
setPromoteMin(uint32_t min) {
  g_promote_min.store(min, std::memory_order_relaxed);
}

void
FrontCounters::publish() {
  if (statsEnabled()) {
    for (unsigned i = 0; i < kCommitWhyCount; i++)
      add(g_commit[i], commit[i]);
    for (unsigned i = 0; i < kPassCloseCount; i++)
      add(g_pass_close[i], pass_close[i]);
    for (unsigned i = 0; i < kPromoteCount; i++)
      add(g_promote[i], promote[i]);
  }
  *this = {};
}

void
EncodeCounters::publishChunk() {
  if (statsEnabled()) {
    add(g_draws, draws);
    add(g_dispatches, dispatches);
    add(g_tile, tile);
    add(g_pso, pso);
    add(g_use_resource, use_resource);
    add(g_blit_cmds, blit_cmds);
    for (unsigned i = 0; i < kEncCount; i++)
      add(g_enc[i], encoders[i]);
    for (unsigned i = 0; i < kRefuseCount; i++)
      add(g_refuse[i], refuse[i]);
    for (unsigned i = 0; i < kAspectCount; i++) {
      add(g_load[i], load_bytes[i]);
      add(g_store[i], store_bytes[i]);
    }
    g_chunks.fetch_add(1, std::memory_order_relaxed);
    add(g_chunk_encoders, chunk_encoders);
    // one producer (the encode thread); the drain resets it to 0
    uint32_t seen = g_chunk_encoders_max.load(std::memory_order_relaxed);
    while (chunk_encoders > seen &&
           !g_chunk_encoders_max.compare_exchange_weak(seen, chunk_encoders, std::memory_order_relaxed)) {
    }
  }
  *this = {};
}

} // namespace dxmt::gw2
