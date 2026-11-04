#include "dxmt_gw2_recipe.hpp"
#include "Metal.hpp"
#include "airconv_public.h"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "winemetal.h"
#include <atomic>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <unordered_set>

// gw2 (#57): see dxmt_gw2_recipe.hpp.
//
// Keys (one table, `cache_<kRecipeSchema>`, in recipes_<kRecipeSchema>.db):
//   'D' + shader SHA-1 (20)                   -> the DXBC bytes
//   'V' + shader SHA-1 + variant digest (40)  -> kind (u8) + the variant's inputs
//   'R' + SHA-1 of the recipe (20)            -> the recipe (its canonical serialization)
//   'M' + "meta"                              -> schema (u32), AIRCONV_VERSION (u32)

namespace dxmt::gw2 {

namespace {

constexpr size_t kMaxQueued = 16384;                 // records
constexpr size_t kMaxQueuedBytes = 128u * 1024 * 1024; // their values

struct Record {
  std::string key;
  std::vector<uint8_t> value;
};

struct Recorder {
  dxmt::mutex mutex;
  dxmt::condition_variable cond;
  std::deque<Record> queue;
  size_t queued_bytes = 0;
  std::unordered_set<std::string> seen; // keys enqueued in this process
  bool started = false;
  // counters (window: exchanged by the section; queue/written totals)
  std::atomic<uint64_t> shaders{0}, variants{0}, recipes{0}, unsupported_variants{0}, unsupported_pipelines{0},
      dropped{0}, written{0}, write_failed{0};
  std::atomic<bool> writer_failed{false};
};

Recorder &
recorder() {
  // Never destroyed: the writer thread is detached and may outlive static destruction.
  static auto *r = new Recorder;
  return *r;
}

void writerThread();

// Enqueues a record once per key and process; false if it was seen or dropped.
bool
enqueue(std::string &&key, std::vector<uint8_t> &&value) {
  auto &r = recorder();
  std::unique_lock<dxmt::mutex> lock(r.mutex);
  if (r.writer_failed.load(std::memory_order_relaxed))
    return false;
  if (!r.seen.insert(key).second)
    return false;
  if (r.queue.size() >= kMaxQueued || r.queued_bytes + value.size() > kMaxQueuedBytes) {
    r.seen.erase(key); // may be recorded later, once there is room
    r.dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  r.queued_bytes += value.size();
  r.queue.push_back({std::move(key), std::move(value)});
  if (!r.started) {
    r.started = true;
    dxmt::thread([] { writerThread(); }).detach();
  }
  lock.unlock();
  r.cond.notify_one();
  return true;
}

void
writerThread() {
  env::setThreadName("dxmt-recipe-writer");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  auto &r = recorder();
  auto path = str::format("dxmt/", env::getExeName(), "/recipes_", kRecipeSchema, ".db");
  auto writer = WMT::CacheWriter::alloc_init(path.c_str(), kRecipeSchema);
  if (writer == nullptr) {
    Logger::warn(str::format("Recipes: can't open ", path, " for writing; recording stops (DXMT_PSO_RECORD)"));
    std::lock_guard<dxmt::mutex> lock(r.mutex);
    r.writer_failed.store(true, std::memory_order_relaxed);
    r.dropped.fetch_add(r.queue.size(), std::memory_order_relaxed);
    r.queue.clear();
    r.queued_bytes = 0;
    return;
  }
  {
    // which schema and codegen the records were made with, for the warming stages
    RecipeBytes meta;
    meta.u32(kRecipeSchema);
    meta.u32(AIRCONV_VERSION);
    std::string key = "Mmeta";
    writer.set(key.data(), key.size(), WMT::MakeDispatchData(meta.data.data(), meta.data.size()));
  }
  Logger::info(str::format("Recipes: recording into $(getconf DARWIN_USER_CACHE_DIR)", path,
                           " (DXMT_PSO_RECORD; schema ", kRecipeSchema, ", airconv ", AIRCONV_VERSION, ")"));
  std::deque<Record> batch;
  while (true) {
    {
      std::unique_lock<dxmt::mutex> lock(r.mutex);
      r.cond.wait(lock, [&] { return !r.queue.empty(); });
      batch.swap(r.queue);
      r.queued_bytes = 0;
    }
    for (auto &rec : batch) {
      // DispatchData copies the bytes (dispatch_data_create, default destructor)
      writer.set(rec.key.data(), rec.key.size(), WMT::MakeDispatchData(rec.value.data(), rec.value.size()));
      r.written.fetch_add(1, std::memory_order_relaxed);
    }
    batch.clear();
  }
}

std::string
section(double, uint32_t) {
  if (!recipeRecording())
    return {};
  auto &r = recorder();
  auto x = [](std::atomic<uint64_t> &v) { return v.exchange(0, std::memory_order_relaxed); };
  uint64_t shaders = x(r.shaders), variants = x(r.variants), recipes = x(r.recipes),
           uv = x(r.unsupported_variants), up = x(r.unsupported_pipelines), dropped = x(r.dropped),
           written = x(r.written);
  size_t queued;
  {
    std::lock_guard<dxmt::mutex> lock(r.mutex);
    queued = r.queue.size();
  }
  if (!shaders && !variants && !recipes && !uv && !up && !dropped && !written && !queued)
    return {};
  return std::format("shaders={} variants={} recipes={} unsupported=variants:{},pipelines:{} written={} queued={} "
                     "dropped={}{}",
                     shaders, variants, recipes, uv, up, written, queued, dropped,
                     r.writer_failed.load(std::memory_order_relaxed) ? " writer=failed" : "");
}

SectionRegistrar g_section("RECORD", section);

std::string
digestKey(char tag, const Sha1Digest &a, const Sha1Digest *b = nullptr) {
  std::string key(1, tag);
  key.append(reinterpret_cast<const char *>(a.data), sizeof(a.data));
  if (b)
    key.append(reinterpret_cast<const char *>(b->data), sizeof(b->data));
  return key;
}

} // namespace

bool
recipeRecording() {
  static const bool on = env::getEnvVar("DXMT_PSO_RECORD") != "0";
  return on;
}

void
recipeShader(const Sha1Digest &sha1, const void *bytecode, size_t length) {
  if (!recipeRecording() || !bytecode || !length)
    return;
  auto *p = static_cast<const uint8_t *>(bytecode);
  if (enqueue(digestKey('D', sha1), std::vector<uint8_t>(p, p + length)))
    recorder().shaders.fetch_add(1, std::memory_order_relaxed);
}

void
recipeVariant(const Sha1Digest &shader, const Sha1Digest &variant, RecipeVariantKind kind, RecipeBytes &&inputs) {
  if (!recipeRecording())
    return;
  RecipeBytes value;
  value.u8(uint8_t(kind));
  value.data.insert(value.data.end(), inputs.data.begin(), inputs.data.end());
  if (enqueue(digestKey('V', shader, &variant), std::move(value.data)))
    recorder().variants.fetch_add(1, std::memory_order_relaxed);
}

void
recipeVariantUnsupported() {
  if (recipeRecording())
    recorder().unsupported_variants.fetch_add(1, std::memory_order_relaxed);
}

void
recipePipeline(const Sha1Digest &vs_shader, const Sha1Digest &vs_variant, const Sha1Digest *ps_shader,
               const Sha1Digest *ps_variant, const WMTRenderPipelineInfo &info) {
  if (!recipeRecording())
    return;
  // The canonical serialization: explicit fields only, in a fixed order.
  RecipeBytes b;
  b.u32(kRecipeSchema);
  b.digest(vs_shader);
  b.digest(vs_variant);
  b.u8(ps_shader && ps_variant);
  if (ps_shader && ps_variant) {
    b.digest(*ps_shader);
    b.digest(*ps_variant);
  }
  for (auto &c : info.colors) {
    b.u32(c.pixel_format);
    b.u32(c.rgb_blend_operation);
    b.u32(c.alpha_blend_operation);
    b.u32(c.src_rgb_blend_factor);
    b.u32(c.dst_rgb_blend_factor);
    b.u32(c.src_alpha_blend_factor);
    b.u32(c.dst_alpha_blend_factor);
    b.u8(c.write_mask);
    b.u8(c.blending_enabled);
  }
  b.u8(info.alpha_to_coverage_enabled);
  b.u8(info.logic_operation_enabled);
  b.u32(info.logic_operation);
  b.u8(info.rasterization_enabled);
  b.u8(info.raster_sample_count);
  b.u32(info.depth_pixel_format);
  b.u32(info.stencil_pixel_format);
  b.u32(info.immutable_vertex_buffers);
  b.u32(info.immutable_fragment_buffers);
  b.u32(info.input_primitive_topology);
  b.u32(info.tessellation_partition_mode);
  b.u8(info.max_tessellation_factor);
  b.u32(info.tessellation_output_winding_order);
  b.u32(info.tessellation_factor_step);
  b.u8(info.support_indirect_command_buffers);
  auto key = Sha1HashState::compute(b.data.data(), b.data.size());
  if (enqueue(digestKey('R', key), std::move(b.data)))
    recorder().recipes.fetch_add(1, std::memory_order_relaxed);
}

void
recipePipelineUnsupported() {
  if (recipeRecording())
    recorder().unsupported_pipelines.fetch_add(1, std::memory_order_relaxed);
}

} // namespace dxmt::gw2
