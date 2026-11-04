#include "dxmt_gw2_subsample.hpp"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace dxmt::gw2 {

namespace {

using counter = std::atomic<uint64_t>;

counter g_candidates, g_accepted, g_upscaled;
std::atomic<uint64_t> g_swapchain_size{0}; // width << 32 | height

std::mutex g_mutex; // the shader table and the sighting set (creation and candidate draws only)
std::unordered_map<std::string, DxbcCounts> g_shaders;
std::unordered_set<uint64_t> g_sightings;
bool g_sightings_full = false;

inline uint64_t
take(counter &total) {
  return total.exchange(0, std::memory_order_relaxed);
}

std::string
subsampleSection(double, uint32_t frames) {
  uint64_t candidates = take(g_candidates), accepted = take(g_accepted), upscaled = take(g_upscaled);
  if (!frames || subsampleMode() == SubsampleMode::Off)
    return {};
  double f = frames;
  return std::format(
      "mode={} candidates={:.2f} accepted={:.2f} upscaled={:.2f}", subsampleModeName(), candidates / f,
      accepted / f, upscaled / f
  );
}

SectionRegistrar s_subsample("SUBSAMPLE", &subsampleSection);

inline uint32_t
rd32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

} // namespace

SubsampleMode
subsampleMode() {
  static const SubsampleMode mode = [] {
    auto v = env::getEnvVar("DXMT_SUBSAMPLE_METALFX");
    SubsampleMode m = SubsampleMode::Off;
    if (v == "1")
      m = SubsampleMode::On;
    else if (v == "log")
      m = SubsampleMode::Log;
    else if (!v.empty() && v != "0")
      Logger::warn(std::format("SUBSAMPLE: DXMT_SUBSAMPLE_METALFX={} is not 0, log or 1; off", v));
    if (m == SubsampleMode::Log)
      Logger::info("SUBSAMPLE: capture only (DXMT_SUBSAMPLE_METALFX=log): candidate passes are logged, nothing "
                   "changes");
    else if (m == SubsampleMode::On)
      Logger::info("SUBSAMPLE: on (DXMT_SUBSAMPLE_METALFX=1): an accepted Subsample pass reads a MetalFX "
                   "spatial upscale of its source");
    return m;
  }();
  return mode;
}

const char *
subsampleModeName() {
  switch (subsampleMode()) {
  case SubsampleMode::Off:
    return "0";
  case SubsampleMode::Log:
    return "log";
  case SubsampleMode::On:
    return "1";
  }
  return "0";
}

const std::string &
subsamplePsPin() {
  static const std::string pin = [] {
    std::string v = env::getEnvVar("DXMT_SUBSAMPLE_METALFX_PS");
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (!v.empty())
      Logger::info(std::format("SUBSAMPLE: only a pass whose pixel shader's SHA-1 starts with {} "
                               "(DXMT_SUBSAMPLE_METALFX_PS)", v));
    return v;
  }();
  return pin;
}

DxbcCounts
scanDxbc(const void *bytecode, size_t length) {
  DxbcCounts c;
  auto *base = static_cast<const uint8_t *>(bytecode);
  if (!base || length < 32 || std::memcmp(base, "DXBC", 4) != 0)
    return c;
  uint32_t chunks = rd32(base + 28);
  if (32 + uint64_t(chunks) * 4 > length)
    return c;
  for (uint32_t i = 0; i < chunks; i++) {
    uint32_t off = rd32(base + 32 + i * 4);
    if (uint64_t(off) + 8 > length)
      return c;
    const uint8_t *chunk = base + off;
    if (std::memcmp(chunk, "SHDR", 4) != 0 && std::memcmp(chunk, "SHEX", 4) != 0)
      continue;
    uint32_t size = rd32(chunk + 4);
    if (uint64_t(off) + 8 + size > length || size < 8)
      return c;
    const uint8_t *data = chunk + 8;
    uint32_t dwords = std::min(rd32(data + 4), size / 4);
    uint32_t pos = 2; // the version and length tokens
    while (pos < dwords) {
      uint32_t token = rd32(data + pos * 4);
      uint32_t opcode = token & 0x7ff;
      uint32_t len = (token >> 24) & 0x7f;
      if (opcode == 53) { // customdata: its length is the next dword
        if (pos + 1 >= dwords)
          return c;
        len = rd32(data + (pos + 1) * 4);
      }
      if (len == 0)
        return c;
      c.total++;
      switch (opcode) {
      case 45: // ld
      case 46: // ld_ms
        c.load++;
        break;
      case 61: // resinfo
        c.resinfo++;
        break;
      case 69: // sample
      case 70: // sample_c
      case 71: // sample_c_lz
      case 72: // sample_l
      case 73: // sample_d
      case 74: // sample_b
      case 109: // gather4
      case 126: // gather4_c
      case 127: // gather4_po
      case 128: // gather4_po_c
        c.sample++;
        break;
      default:
        break;
      }
      pos += len;
    }
    c.parsed = true;
    return c;
  }
  return c;
}

void
notePixelShader(const std::string &sha1, const void *bytecode, size_t length) {
  auto counts = scanDxbc(bytecode, length);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_shaders.emplace(sha1, counts);
}

DxbcCounts
pixelShaderCounts(const std::string &sha1) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_shaders.find(sha1);
  return it == g_shaders.end() ? DxbcCounts{} : it->second;
}

void
noteSwapchainSize(uint32_t width, uint32_t height) {
  g_swapchain_size.store(uint64_t(width) << 32 | height, std::memory_order_relaxed);
}

void
swapchainSize(uint32_t &width, uint32_t &height) {
  uint64_t v = g_swapchain_size.load(std::memory_order_relaxed);
  width = uint32_t(v >> 32);
  height = uint32_t(v);
}

bool
subsampleFirstSighting(uint64_t key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_sightings.count(key))
    return false;
  if (g_sightings.size() >= 64) {
    if (!g_sightings_full) {
      g_sightings_full = true;
      Logger::info("SUBSAMPLE: 64 candidate shapes logged; no more candidate lines");
    }
    return false;
  }
  g_sightings.insert(key);
  return true;
}

void
subsampleCandidate(bool accepted) {
  if (!statsEnabled())
    return;
  g_candidates.fetch_add(1, std::memory_order_relaxed);
  if (accepted)
    g_accepted.fetch_add(1, std::memory_order_relaxed);
}

void
subsampleUpscaled() {
  if (statsEnabled())
    g_upscaled.fetch_add(1, std::memory_order_relaxed);
}

} // namespace dxmt::gw2
