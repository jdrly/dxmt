#include "dxmt_gw2_recipe.hpp"
#include "dxmt_compile_recipe.hpp"
#include "Metal.hpp"
#include "airconv_public.h"
#include "dxmt_gw2_recipe_use.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include "dxmt_gw2_warm.hpp"
#include "dxmt_gw2_warm_bridge.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "winemetal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <string>

// gw2 (#57): see dxmt_gw2_recipe.hpp.
//
// Keys (one table, `cache_<kRecipeSchema>`, in recipes_<kRecipeSchema>.db). Every key is content
// addressed (the same key always holds the same value), so a key already in the file is skipped:
//   'D' + shader SHA-1 (20)                              -> the DXBC bytes
//   'V' + shader SHA-1 + variant digest (40) + target (4) -> schema (u32), target (u32), airconv (u32),
//                                                           kind (u8), the variant's inputs
//   'R' + SHA-1 of the recipe (20)                       -> the recipe: schema (u32), target (u32), ...
//   'U' + SHA-1 of the recipe (20)                       -> schema, lastSession, sessionsUsed, firstUseMs
//                                                           (u32 each; #124, rewritten in place)
//   "Ssession"                                           -> schema, the last session's number (#124)
// (dxmt_gw2_recipe_use.hpp). A U row is the one row the writer replaces: it merges the session's use.
// The target (SM50_SHADER_METAL_VERSION) is a translation input, so it is part of the V identity and
// of every recipe. AIRCONV_VERSION isn't: a warm stage translates the DXBC with its own airconv, so a
// V row only notes the airconv it was first recorded with.
//
// The budget is the file's own size (SQLite's page_count * page_size, from the bridge): read when
// the writer opens it, and again after every batch; within a batch the rows' keys and values are
// added to it. Past the budget, recording stops for the process (the file keeps what it has).

namespace dxmt::gw2 {

namespace {

constexpr size_t kMaxQueued = 16384;                   // records waiting for the writer
constexpr size_t kMaxQueuedBytes = 128u * 1024 * 1024;  // their keys and values
constexpr size_t kDedupSlots = size_t(1) << 18;         // 2 MB of fingerprints, fixed
constexpr unsigned kDedupProbe = 64;                    // slots tried before a key counts as overflow
constexpr uint64_t kBudgetBytes = 256ull * 1024 * 1024; // the file's size
constexpr unsigned kMaxConsecutiveFailures = 8;         // failed writes in a row before recording stops
constexpr size_t kMaxQueuedLines = 16;
constexpr DWORD kPollMs = 200;

struct Node {
  Node *next;
  std::string key;
  std::vector<uint8_t> value;
};

enum class Stop : uint32_t { None, WriterFailed, Budget };

struct QueuedLine {
  bool warn;
  std::string text;
};

struct Recorder {
  // what this process has admitted: 64-bit fingerprints of the keys (bit 0 clear), bit 0 set while
  // the record is admitted (queued, or stored). Fixed size: no allocation or rehash, no lock.
  std::unique_ptr<std::atomic<uint64_t>[]> seen{new std::atomic<uint64_t>[kDedupSlots]()};
  std::atomic<Node *> head{nullptr}; // the producers' list (newest first); the writer takes it whole
  std::atomic<size_t> queued{0}, queued_bytes{0};
  std::atomic<bool> started{false};
  std::atomic<Stop> stop{Stop::None};
  // counters: exchanged by the RECORD section
  std::atomic<uint64_t> shaders{0}, variants{0}, recipes{0}, unsupported_variants{0}, unsupported_pipelines{0},
      written{0}, present{0}, failed{0}, dropped_queue{0}, dropped_seen{0}, dropped_stop{0};
  std::atomic<uint64_t> uses{0}; // gw2 (#124): use rows written (merged)

  // gw2 (#124): the session's start on the steady clock, for the first-use times. The recorder is
  // created by the process's first record. The session's number is the writer's (UseLedger).
  const std::chrono::steady_clock::time_point session_start = std::chrono::steady_clock::now();

  // the writer thread's log lines (it never calls the logger)
  dxmt::mutex lines_mutex;
  std::vector<QueuedLine> lines; // lines_mutex
  uint32_t lines_dropped = 0;    // lines_mutex
  std::atomic<bool> lines_pending{false};

  void
  post(bool warn, std::string text) {
    std::lock_guard<dxmt::mutex> lock(lines_mutex);
    if (lines.size() < kMaxQueuedLines)
      lines.push_back({warn, std::move(text)});
    else
      lines_dropped++;
    lines_pending.store(true, std::memory_order_release);
  }
};

std::atomic<Recorder *> g_recorder{nullptr}; // set once the recorder exists

Recorder &
recorder() {
  // Never destroyed: the writer thread is detached and may outlive static destruction.
  static auto *r = [] {
    auto *p = new Recorder;
    g_recorder.store(p, std::memory_order_release);
    return p;
  }();
  return *r;
}

// A key's 63-bit fingerprint (FNV-1a; bit 0 is the admitted flag). Two different keys with the same
// fingerprint count as one (#58 review, accepted; #103): the table keeps no keys, which would cost
// about 12 MB at 2^18 slots. The keys are SHA-1-based (21-45 bytes), so for n = 30,000 keys a session
// the chance of any collision is about n^2 / 2^64, about 5e-11. Fingerprints are deterministic, so a
// colliding pair collides in every session that sees both: the second key's row is never recorded
// and its pipeline is built at first use, as without recording. Warming's own tables (C4) are keyed
// by full digests and never rely on this.
uint64_t
fingerprint(const std::string &key) {
  uint64_t h = 1469598103934665603ull; // FNV-1a 64
  for (unsigned char c : key) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return (h | 2) & ~uint64_t(1); // never 0, bit 0 free for the admitted flag
}

enum class Claim { New, Seen, Overflow };

// Admits a key once per process: New if the caller now owns it (also a key whose earlier record
// was dropped or failed), Seen if it is already admitted, Overflow if its probe run is full.
Claim
claim(Recorder &r, const std::string &key) {
  uint64_t fp = fingerprint(key);
  for (unsigned i = 0; i < kDedupProbe; i++) {
    auto &slot = r.seen[((fp >> 1) + i) & (kDedupSlots - 1)];
    uint64_t v = slot.load(std::memory_order_acquire);
    while (true) {
      if (v == 0) {
        if (slot.compare_exchange_weak(v, fp | 1, std::memory_order_acq_rel))
          return Claim::New;
        continue; // v reloaded
      }
      if ((v & ~uint64_t(1)) != fp)
        break; // another key's slot: probe on
      if (v & 1)
        return Claim::Seen;
      if (slot.compare_exchange_weak(v, fp | 1, std::memory_order_acq_rel))
        return Claim::New;
    }
  }
  return Claim::Overflow;
}

// The record for `key` wasn't stored: a later sighting may admit it again.
void
unclaim(Recorder &r, const std::string &key) {
  uint64_t fp = fingerprint(key);
  for (unsigned i = 0; i < kDedupProbe; i++) {
    auto &slot = r.seen[((fp >> 1) + i) & (kDedupSlots - 1)];
    uint64_t v = slot.load(std::memory_order_acquire);
    if (v == 0)
      return;
    if ((v & ~uint64_t(1)) == fp) {
      slot.store(fp, std::memory_order_release);
      return;
    }
  }
}

void writerThread();

enum class Admit { Queued, Seen, Dropped };

// Never blocks: atomics, the record's own allocation, and once per process the writer's creation.
Admit
enqueue(std::string &&key, std::vector<uint8_t> &&value) {
  auto &r = recorder();
  if (r.stop.load(std::memory_order_relaxed) != Stop::None) {
    r.dropped_stop.fetch_add(1, std::memory_order_relaxed);
    return Admit::Dropped;
  }
  switch (claim(r, key)) {
  case Claim::Seen:
    return Admit::Seen;
  case Claim::Overflow:
    r.dropped_seen.fetch_add(1, std::memory_order_relaxed);
    return Admit::Dropped;
  case Claim::New:
    break;
  }
  size_t bytes = key.size() + value.size();
  size_t n = r.queued.fetch_add(1, std::memory_order_relaxed);
  size_t b = r.queued_bytes.fetch_add(bytes, std::memory_order_relaxed);
  if (n >= kMaxQueued || b + bytes > kMaxQueuedBytes) {
    r.queued.fetch_sub(1, std::memory_order_relaxed);
    r.queued_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    unclaim(r, key);
    r.dropped_queue.fetch_add(1, std::memory_order_relaxed);
    return Admit::Dropped;
  }
  auto *node = new Node{nullptr, std::move(key), std::move(value)};
  node->next = r.head.load(std::memory_order_relaxed);
  while (!r.head.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed))
    ;
  if (!r.started.exchange(true, std::memory_order_relaxed))
    dxmt::thread([] { writerThread(); }).detach();
  return Admit::Queued;
}

// The writer's batch, oldest first; the queue counters are released as it is taken.
Node *
takeAll(Recorder &r) {
  Node *list = r.head.exchange(nullptr, std::memory_order_acquire);
  Node *oldest = nullptr;
  while (list) {
    Node *next = list->next;
    list->next = oldest;
    oldest = list;
    list = next;
  }
  return oldest;
}

void
release(Recorder &r, Node *node) {
  r.queued.fetch_sub(1, std::memory_order_relaxed);
  r.queued_bytes.fetch_sub(node->key.size() + node->value.size(), std::memory_order_relaxed);
  delete node;
}

void
stopRecording(Recorder &r, Stop why, std::string line) {
  r.stop.store(why, std::memory_order_relaxed);
  r.post(true, std::move(line));
}

void
writerThread() {
  env::setThreadName("dxmt-recipe-writer");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  auto &r = recorder();
  auto path = str::format("dxmt/", env::getExeName(), "/recipes_", kRecipeSchema, ".db");
  auto shown = str::format("$(getconf DARWIN_USER_CACHE_DIR)", path);

  // The writer creates the file and its table; the reader then finds the rows already stored.
  auto writer = WMT::CacheWriter::alloc_init(path.c_str(), kRecipeSchema);
  auto reader = writer == nullptr ? WMT::Reference<WMT::CacheReader>()
                                  : WMT::CacheReader::alloc_init(path.c_str(), kRecipeSchema);
  uint64_t accounted = writer == nullptr ? 0 : writer.byteSize();
  if (writer == nullptr || reader == nullptr || accounted == 0) {
    stopRecording(r, Stop::WriterFailed,
                  str::format("Recipes: can't open ", shown, "; recording stops (DXMT_PSO_RECORD)"));
  } else {
    if (accounted >= kBudgetBytes)
      stopRecording(r, Stop::Budget,
                    str::format("Recipes: ", shown, " holds ", accounted >> 20, " MB, its budget is ",
                                kBudgetBytes >> 20, " MB; recording stops. Delete the file to start over"));
    else
      r.post(false, std::format("Recipes: recording into {} (DXMT_PSO_RECORD; schema {}, airconv {}, {} of {} MB)",
                                shown, kRecipeSchema, AIRCONV_VERSION, accounted >> 20, kBudgetBytes >> 20));
  }

  // gw2 (#124): the use data: every U row and the session counter, then this session's number,
  // written back at once (a newer session always gets a larger number)
  UseLedger ledger;
  bool use_ok = false;
  if (recipeUseRecording() && r.stop.load(std::memory_order_relaxed) == Stop::None) {
    std::atomic<bool> never{false};
    auto load = [&](const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl) { ledger.load(k, kl, v, vl); };
    uint32_t err = warmReadRows(warmEnumeratePrefix(path, kUseTag), never, load);
    if (!err)
      err = warmReadRows(warmEnumeratePrefix(path, kSessionTag), never, load);
    uint32_t session = ledger.begin();
    auto value = sessionValue(session);
    if (!err && writer.set(kSessionKey.data(), kSessionKey.size(), WMT::MakeDispatchData(value.data(), value.size()))) {
      use_ok = true;
      r.post(false, std::format("Recipes: session {} ({} recipes with use data; DXMT_PSO_RECORD_USE)", session,
                                ledger.recipes.size()));
    } else {
      r.post(true, std::format("Recipes: can't read or start the use data (error {}); no use rows this session "
                               "(DXMT_PSO_RECORD_USE)",
                               err));
    }
  }

  unsigned consecutive_failures = 0;
  while (true) {
    Node *node = takeAll(r);
    if (!node) {
      Sleep(kPollMs);
      continue;
    }
    bool stored_any = false;
    while (node) {
      Node *next = node->next;
      std::vector<uint8_t> use_value;
      bool use_row = node->key.size() == kUseKeyBytes && node->key[0] == kUseTag && node->value.size() == 4;
      if (r.stop.load(std::memory_order_relaxed) != Stop::None) {
        r.dropped_stop.fetch_add(1, std::memory_order_relaxed);
      } else if (use_row) {
        // gw2 (#124): merge this session's use into the recipe's one U row (INSERT OR REPLACE)
        uint32_t first_ms = uint32_t(node->value[0]) | uint32_t(node->value[1]) << 8 |
                            uint32_t(node->value[2]) << 16 | uint32_t(node->value[3]) << 24;
        std::string id = node->key.substr(1);
        bool known = ledger.recipes.count(id) != 0;
        if (!use_ok || !ledger.note(id, first_ms, use_value)) {
          r.present.fetch_add(1, std::memory_order_relaxed); // counted already, or no use data this session
        } else if (!known && accounted + node->key.size() + use_value.size() > kBudgetBytes) {
          r.dropped_stop.fetch_add(1, std::memory_order_relaxed);
          stopRecording(r, Stop::Budget,
                        str::format("Recipes: ", shown, " reached its budget (", kBudgetBytes >> 20,
                                    " MB); recording stops. Delete the file to start over"));
        } else if (writer.set(node->key.data(), node->key.size(),
                              WMT::MakeDispatchData(use_value.data(), use_value.size()))) {
          if (!known)
            accounted += node->key.size() + use_value.size();
          stored_any = true;
          consecutive_failures = 0;
          r.uses.fetch_add(1, std::memory_order_relaxed);
        } else {
          r.failed.fetch_add(1, std::memory_order_relaxed);
          if (++consecutive_failures >= kMaxConsecutiveFailures)
            stopRecording(r, Stop::WriterFailed,
                          str::format("Recipes: ", kMaxConsecutiveFailures, " writes in a row into ", shown,
                                      " failed; recording stops (DXMT_PSO_RECORD)"));
        }
      } else if (reader.get(node->key.data(), node->key.size()) != nullptr) {
        r.present.fetch_add(1, std::memory_order_relaxed); // an earlier session stored it
      } else if (accounted + node->key.size() + node->value.size() > kBudgetBytes) {
        r.dropped_stop.fetch_add(1, std::memory_order_relaxed);
        stopRecording(r, Stop::Budget,
                      str::format("Recipes: ", shown, " reached its budget (", kBudgetBytes >> 20,
                                  " MB); recording stops. Delete the file to start over"));
      } else if (writer.set(node->key.data(), node->key.size(),
                            // DispatchData copies the bytes (dispatch_data_create, default destructor)
                            WMT::MakeDispatchData(node->value.data(), node->value.size()))) {
        accounted += node->key.size() + node->value.size();
        stored_any = true;
        consecutive_failures = 0;
        r.written.fetch_add(1, std::memory_order_relaxed);
      } else {
        r.failed.fetch_add(1, std::memory_order_relaxed);
        unclaim(r, node->key);
        if (++consecutive_failures >= kMaxConsecutiveFailures)
          stopRecording(r, Stop::WriterFailed,
                        str::format("Recipes: ", kMaxConsecutiveFailures, " writes in a row into ", shown,
                                    " failed; recording stops (DXMT_PSO_RECORD)"));
      }
      release(r, node);
      node = next;
    }
    if (stored_any) {
      if (uint64_t size = writer.byteSize())
        accounted = size; // the file's real size again
    }
  }
}

std::string
section(double, uint32_t) {
  auto *rp = g_recorder.load(std::memory_order_acquire);
  if (!rp)
    return {}; // nothing recorded yet (or DXMT_PSO_RECORD=0)
  auto &r = *rp;
  auto x = [](std::atomic<uint64_t> &v) { return v.exchange(0, std::memory_order_relaxed); };
  uint64_t shaders = x(r.shaders), variants = x(r.variants), recipes = x(r.recipes),
           uv = x(r.unsupported_variants), up = x(r.unsupported_pipelines), written = x(r.written),
           present = x(r.present), failed = x(r.failed), dq = x(r.dropped_queue), ds = x(r.dropped_seen),
           dst = x(r.dropped_stop), uses = x(r.uses);
  size_t queued = r.queued.load(std::memory_order_relaxed);
  if (!shaders && !variants && !recipes && !uv && !up && !written && !present && !failed && !dq && !ds && !dst &&
      !queued && !uses)
    return {};
  auto stop = r.stop.load(std::memory_order_relaxed);
  return std::format("shaders={} variants={} recipes={} unsupported=variants:{},pipelines:{} written={} present={} "
                     "failed={} queued={} dropped=queue:{},seen:{},stopped:{}{}",
                     shaders, variants, recipes, uv, up, written, present, failed, queued, dq, ds, dst,
                     stop == Stop::Budget ? " stopped=budget" : stop == Stop::WriterFailed ? " stopped=writer" : "") +
         std::format(" uses={}", uses); // gw2 (#124)
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

// gw2 (#124): one use per recipe and process (the recorder's per-process dedup admits its key once);
// the writer merges it into the recipe's U row. The queued value is the first use (u32 ms) only.
void
noteUse(const Sha1Digest &recipe_key) {
  if (!recipeUseRecording())
    return;
  auto &r = recorder();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - r.session_start)
                .count();
  uint32_t first_ms = uint32_t(std::min<int64_t>(std::max<int64_t>(ms, 0), int64_t(UINT32_MAX)));
  std::vector<uint8_t> value;
  for (int i = 0; i < 4; i++)
    value.push_back(uint8_t(first_ms >> (8 * i)));
  enqueue(recipeUseKey(recipeUseId(recipe_key)), std::move(value));
}

// gw2 (#124): warm hits note their use through warming's seam (warmPipelineHit)
struct InstallHitUse {
  InstallHitUse() { setWarmHitUse(&recipeUse); }
} g_install_hit_use;

} // namespace

bool
recipeRecording() {
  static const bool on = env::getEnvVar("DXMT_PSO_RECORD") != "0";
  return on;
}

// gw2 (#124): DXMT_PSO_RECORD_USE (on unless "0"), with recording on
namespace {
sw::Switch<bool> kRecordUse{"DXMT_PSO_RECORD_USE", true, sw::legacy::notZero};
}

bool
recipeUseRecording() {
  static const bool on = recipeRecording() && kRecordUse();
  return on;
}

void
recipeShader(const Sha1Digest &sha1, const void *bytecode, size_t length) {
  if (!recipeRecording() || !bytecode || !length)
    return;
  auto *p = static_cast<const uint8_t *>(bytecode);
  if (enqueue(digestKey('D', sha1), std::vector<uint8_t>(p, p + length)) == Admit::Queued)
    recorder().shaders.fetch_add(1, std::memory_order_relaxed);
}

bool
recipeVariant(const Sha1Digest &shader, const Sha1Digest &variant, uint32_t metal_version, RecipeVariantKind kind,
              RecipeBytes &&inputs) {
  if (!recipeRecording())
    return false;
  RecipeBytes value;
  value.u32(kRecipeSchema);
  value.u32(metal_version);
  value.u32(AIRCONV_VERSION);
  value.u8(uint8_t(kind));
  value.data.insert(value.data.end(), inputs.data.begin(), inputs.data.end());
  auto key = digestKey('V', shader, &variant);
  for (int i = 0; i < 4; i++)
    key.push_back(char(uint8_t(metal_version >> (8 * i))));
  switch (enqueue(std::move(key), std::move(value.data))) {
  case Admit::Queued:
    recorder().variants.fetch_add(1, std::memory_order_relaxed);
    return true;
  case Admit::Seen:
    return true;
  case Admit::Dropped:
    return false;
  }
  return false;
}

void
recipeVariantUnsupported() {
  if (recipeRecording())
    recorder().unsupported_variants.fetch_add(1, std::memory_order_relaxed);
}

void
recipePipeline(const compile::PipelineRecipe &recipe) {
  if (!recipeRecording() || !recipe.vs)
    return;
  // The canonical serialization (C3: compile::serialize, the format #57 introduced)
  RecipeBytes b;
  compile::serialize(recipe, b);
  auto key = Sha1HashState::compute(b.data.data(), b.data.size());
  if (enqueue(digestKey('R', key), std::move(b.data)) == Admit::Queued)
    recorder().recipes.fetch_add(1, std::memory_order_relaxed);
  noteUse(key); // gw2 (#124)
}

void
recipeUse(const Sha1Digest &recipe_key) {
  if (recipeRecording())
    noteUse(recipe_key);
}

void
recipePipelineUnsupported() {
  if (recipeRecording())
    recorder().unsupported_pipelines.fetch_add(1, std::memory_order_relaxed);
}

void
recipeLogPending() {
  auto *rp = g_recorder.load(std::memory_order_acquire);
  if (!rp)
    return;
  auto &r = *rp;
  if (!r.lines_pending.load(std::memory_order_acquire))
    return;
  if (!r.lines_mutex.try_lock())
    return; // the writer is posting a line; the next Present takes it
  std::vector<QueuedLine> lines;
  lines.swap(r.lines);
  uint32_t dropped = r.lines_dropped;
  r.lines_dropped = 0;
  r.lines_pending.store(false, std::memory_order_relaxed);
  r.lines_mutex.unlock();
  for (auto &line : lines) {
    if (line.warn)
      Logger::warn(line.text);
    else
      Logger::info(line.text);
  }
  if (dropped)
    Logger::warn(std::format("Recipes: {} lines dropped", dropped));
}

} // namespace dxmt::gw2
