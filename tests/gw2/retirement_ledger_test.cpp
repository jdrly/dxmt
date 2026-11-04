// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O1 -Wall -Wextra -Isrc/dxmt tests/gw2/retirement_ledger_test.cpp -o $OUT
//
// gw2 (#66 L1, #69 L2, architecture council §3): native test of the retirement ledger
// (src/dxmt/dxmt_gw2_retirement.hpp) with counting owners: every owner logs its release, so a test
// checks what was released, when and in which order. No Metal, no Wine, no threads.
//
// - The ledger's rules: nothing released before its chunk retires; the cross-category order; ring wrap
//   at 32, 64 and 128 with a stale generation refused; two queues with the same sequence; repeated,
//   skipped and post-drain retirements refused; no hold after retirement began (even from a release);
//   null holds; teardown's slot-by-slot drain.
// - Parity: scripted encode/finish runs (the production ring discipline: at most ring - 1 chunks in
//   flight, retirements in order, chunk resets around them) against a copy of what it replaced
//   (gw2-on-mac 74a7836's CommandChunk keep lists, split_cmdbufs, reset() and teardown loop, the
//   "legacy oracle" below). The release traces must be identical, teardown included.
// - Policy parity (#69, L2): the retention policy against each call site's former gate (5f16916) in
//   every reachable configuration (Metal 3 retained or unretained, the lifetime check, the residency
//   set, Metal 4), and the scripted parity runs once per configuration with those gates.
//
// Run: tests/gw2/run.zsh pure, or the command above with $OUT set.

#include "dxmt_gw2_retirement.hpp"
#include <array>
#include <cstdio>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;
#define CHECK(...)                                                                                 \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(__VA_ARGS__)) {                                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// --- counting owners

static std::vector<std::string> g_released;              // every release, in order
static std::function<void(const std::string &)> g_on_release; // a hook, for re-entrant holds

static void
logRelease(const std::string &name) {
  g_released.push_back(name);
  if (g_on_release) {
    auto hook = std::move(g_on_release); // one shot, and not re-entered
    g_on_release = nullptr;
    hook(name);
  }
}

// a move-only owner of a named thing; releases it (logs it) when destroyed or overwritten
template <int Category> struct Owner {
  std::string name;
  Owner() = default;
  explicit Owner(std::string n) : name(std::move(n)) {}
  Owner(Owner &&o) noexcept : name(std::exchange(o.name, {})) {}
  Owner &
  operator=(Owner &&o) noexcept {
    reset();
    name = std::exchange(o.name, {});
    return *this;
  }
  Owner(const Owner &) = delete;
  Owner &operator=(const Owner &) = delete;
  ~Owner() { reset(); }
  void
  reset() {
    if (!name.empty())
      logRelease(std::exchange(name, {}));
  }
  explicit operator bool() const { return !name.empty(); }
};

using CmdBuf = Owner<0>;
using Pres = Owner<1>;
using Spatial = Owner<2>;
using Temporal = Owner<3>;

// objects are raw handles (as obj_handle_t): an index into this table of names
static std::vector<std::string> g_objects{""};
static uint64_t
object(std::string name) {
  g_objects.push_back(std::move(name));
  return g_objects.size() - 1;
}

struct TestTraits {
  using Object = uint64_t;
  static void
  release(uint64_t o) {
    logRelease(g_objects[o]);
  }
  using CommandBuffer = CmdBuf;
  using Presenter = Pres;
  using SpatialScaler = Spatial;
  using TemporalScaler = Temporal;
};
using Ledger = dxmt::gw2::BasicRetirementLedger<TestTraits>;
using dxmt::gw2::HoldReason;
using dxmt::gw2::RetentionPolicy;

// the rule tests hold everything: the policy of DXMT_UNRETAINED=1 (the default)
static const RetentionPolicy kAll{.unretained = true};

static std::vector<std::string>
take() {
  return std::exchange(g_released, {});
}

static std::string
str(const std::vector<std::string> &v) {
  std::string s;
  for (auto &x : v)
    s += (s.empty() ? "" : ",") + x;
  return s;
}

#define CHECK_RELEASED(...)                                                                        \
  do {                                                                                             \
    auto got_ = take();                                                                            \
    std::vector<std::string> want_ = __VA_ARGS__;                                                  \
    checks++;                                                                                      \
    if (got_ != want_) {                                                                           \
      std::printf("FAIL %s:%d: released [%s], expected [%s]\n", __FILE__, __LINE__, str(got_).c_str(), \
                  str(want_).c_str());                                                             \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// holds one of each category for chunk `seq` of `l`, in an order unlike the release order
static void
holdAll(Ledger &l, uint64_t seq, const std::string &tag = "") {
  auto n = [&](const char *what) { return std::string(what) + tag + std::to_string(seq); };
  CHECK(l.hold(seq, Temporal(n("temporal"))));
  CHECK(l.hold(seq, Spatial(n("spatial"))));
  CHECK(l.hold(seq, Pres(n("presenter"))));
  CHECK(l.hold(seq, object(n("object")), HoldReason::Event));
  CHECK(l.holdSplit(seq, CmdBuf(n("split"))));
}

static std::vector<std::string>
allReleased(uint64_t seq, const std::string &tag = "") {
  auto n = [&](const char *what) { return std::string(what) + tag + std::to_string(seq); };
  return {n("split"), n("object"), n("presenter"), n("spatial"), n("temporal")};
}

// --- the ledger's rules

static void
testNothingBeforeRetire() {
  Ledger l(32, kAll);
  for (uint64_t s = 1; s <= 3; s++)
    holdAll(l, s);
  CHECK_RELEASED({});
  // the finish thread reads the split buffers before it retires
  CHECK(l.splits(2).size() == 1 && l.splits(2)[0].name == "split2");
  CHECK(l.retire(1));
  CHECK_RELEASED(allReleased(1)); // chunk 1's, and nothing of 2 or 3
  CHECK(l.retire(2));
  CHECK_RELEASED(allReleased(2));
  CHECK(l.retire(3));
  CHECK_RELEASED(allReleased(3));
  CHECK(l.retired() == 3);
}

static void
testOrder() {
  Ledger l(32, kAll);
  // several of each, interleaved: objects in hold order; the owner lists in their vector's clear()
  // order (as the chunk's lists were), so one each of those
  CHECK(l.hold(1, Temporal("t")));
  CHECK(l.hold(1, object("o1"), HoldReason::Event));
  CHECK(l.holdSplit(1, CmdBuf("s1")));
  CHECK(l.hold(1, Pres("p")));
  CHECK(l.hold(1, object("o2"), HoldReason::Event));
  CHECK(l.hold(1, Spatial("sp")));
  CHECK(l.holdSplit(1, CmdBuf("s2")));
  CHECK(l.hold(1, object("o3"), HoldReason::Event));
  CHECK(l.splits(1).size() == 2 && l.splits(1)[0].name == "s1" && l.splits(1)[1].name == "s2");
  CHECK(l.retire(1));
  auto got = take();
  // splits first (both, either order), then o1 o2 o3, then p, sp, t
  CHECK(got.size() == 8);
  if (got.size() == 8) {
    CHECK((got[0] == "s1" && got[1] == "s2") || (got[0] == "s2" && got[1] == "s1"));
    CHECK(got[2] == "o1" && got[3] == "o2" && got[4] == "o3");
    CHECK(got[5] == "p" && got[6] == "sp" && got[7] == "t");
  }
}

static void
testNullHolds() {
  Ledger l(32, kAll);
  CHECK(l.hold(1, uint64_t(0), HoldReason::Drawable));
  Pres empty;
  CHECK(l.hold(1, std::move(empty)));
  CHECK(l.hold(1, Spatial()));
  CHECK(l.hold(1, Temporal()));
  Pres p("p1");
  CHECK(l.hold(1, std::move(p)));
  CHECK(!p); // taken over
  CHECK(l.retire(1));
  CHECK_RELEASED({"p1"});
  // a null hold doesn't claim a slot: a stale chunk's slot stays free for its owner
  Ledger m(32, kAll);
  CHECK(m.hold(33, Pres())); // null: no claim on slot 1
  CHECK(m.hold(1, Pres("x")));
  CHECK(m.retire(1));
  CHECK_RELEASED({"x"});
}

static void
testRingWrap(uint32_t n) {
  Ledger l(n, kAll);
  // the production discipline: at most n - 1 chunks in flight; retirements in sequence
  uint64_t retired = 0;
  for (uint64_t s = 1; s <= 5 * n; s++) {
    if (s - retired > n - 1) {
      CHECK(l.retire(++retired));
      if (retired % 3)
        CHECK_RELEASED(allReleased(retired));
      else
        CHECK_RELEASED({});
    }
    if (s % 3) // some chunks hold nothing
      holdAll(l, s);
    else
      CHECK(l.splits(s).empty());
  }
  // a stale generation: slot of chunk s (unretired) refuses chunk s + n, which is kept until drain
  uint64_t s = retired + 1;
  while (s % 3 == 0)
    s++;
  CHECK(l.hold(s + n, Pres("stale")) == false);
  CHECK(l.holdSplit(s + n, CmdBuf("stalesplit")) == false);
  CHECK(l.hold(s + n, object("staleobject"), HoldReason::Event) == false);
  CHECK(l.splits(s + n).empty()); // the slot is chunk s's
  CHECK(l.splits(s).size() == 1);
  CHECK_RELEASED({});
  while (retired < 5 * n) {
    CHECK(l.retire(++retired));
    if (retired % 3)
      CHECK_RELEASED(allReleased(retired));
    else
      CHECK_RELEASED({});
  }
  // the refused holds only now, after every slot
  l.drain();
  CHECK_RELEASED({"stalesplit", "staleobject", "stale"});
  // after a retirement the slot takes its next chunk
  Ledger m(n, kAll);
  CHECK(m.hold(1, Pres("a")));
  CHECK(m.retire(1));
  CHECK_RELEASED({"a"});
  CHECK(m.hold(1 + n, Pres("b"))); // same slot, the previous chunk retired
  for (uint64_t r = 2; r <= n + 1; r++)
    CHECK(m.retire(r));
  CHECK_RELEASED({"b"});
}

static void
testTwoQueues() {
  Ledger a(32, kAll), b(64, kAll);
  holdAll(a, 1, "a");
  holdAll(b, 1, "b");
  CHECK(a.retire(1));
  CHECK_RELEASED(allReleased(1, "a"));
  CHECK(b.retired() == 0);
  CHECK(b.retire(1));
  CHECK_RELEASED(allReleased(1, "b"));
}

static void
testRefusedRetirements() {
  Ledger l(32, kAll);
  holdAll(l, 1);
  holdAll(l, 2);
  CHECK(!l.retire(0));
  CHECK(!l.retire(2)); // skipped 1
  CHECK_RELEASED({});
  CHECK(l.retire(1));
  CHECK_RELEASED(allReleased(1));
  CHECK(!l.retire(1)); // repeated
  CHECK_RELEASED({});
  CHECK(l.retired() == 1);
  l.drain();
  CHECK_RELEASED(allReleased(2));
  CHECK(!l.retire(2)); // after drain
  CHECK_RELEASED({});
}

static void
testNoHoldAfterRetirementBegan() {
  {
    Ledger l(32, kAll);
    CHECK(l.hold(1, Pres("p1")));
    CHECK(l.retire(1));
    CHECK_RELEASED({"p1"});
    CHECK(!l.hold(1, Pres("late"))); // refused, and not released now
    CHECK(!l.holdSplit(1, CmdBuf("latesplit")));
    CHECK_RELEASED({});
    CHECK(l.hold(2, Pres("p2")));
    CHECK(l.retire(2));
    CHECK_RELEASED({"p2"});
    l.drain();
    CHECK_RELEASED({"latesplit", "late"});
  }
  {
    // a release that holds again for the chunk being retired: refused, kept, released at drain
    Ledger l(32, kAll);
    CHECK(l.hold(1, object("o1"), HoldReason::Event));
    CHECK(l.hold(1, Pres("p1")));
    bool refused = false;
    g_on_release = [&](const std::string &) { refused = !l.hold(1, Spatial("reentrant")); };
    CHECK(l.retire(1));
    CHECK(refused);
    CHECK_RELEASED({"o1", "p1"});
    l.drain();
    CHECK_RELEASED({"reentrant"});
  }
  {
    // holds after drain are refused; the ledger's destructor releases them
    {
      Ledger l(32, kAll);
      l.drain();
      CHECK(!l.hold(1, Pres("afterdrain")));
      CHECK_RELEASED({});
    }
    CHECK_RELEASED({"afterdrain"});
  }
}

static void
testDrainOrder() {
  Ledger l(4, kAll);
  // chunks 1-3 in flight (slots 1, 2, 3); 4 (slot 0) too after 1 retired; a refused one
  holdAll(l, 1);
  holdAll(l, 2);
  CHECK(l.retire(1));
  take();
  holdAll(l, 3);
  holdAll(l, 4);
  CHECK(!l.hold(6, Pres("refused6"))); // slot 2 is chunk 2's
  l.drain([](uint32_t slot) { g_released.push_back("reset" + std::to_string(slot)); });
  std::vector<std::string> want{"reset0"};
  for (auto &x : allReleased(4))
    want.push_back(x);
  want.push_back("reset1"); // chunk 1 retired: nothing left
  want.push_back("reset2");
  for (auto &x : allReleased(2))
    want.push_back(x);
  want.push_back("reset3");
  for (auto &x : allReleased(3))
    want.push_back(x);
  want.push_back("refused6");
  CHECK_RELEASED(want);
}

// --- parity with what the ledger replaced

// The configurations a queue runs in: Metal 3 with retained or unretained command buffers, with or
// without the lifetime check and the residency set, and Metal 4 (which runs only with DXMT_UNRETAINED=1
// or the check, and the residency set: dxmt_gw2_metal4.cpp's metal4AttachQueue refuses otherwise).
struct Config {
  bool metal4, unretained, check, residency;
  bool
  reachable() const {
    return !metal4 || ((unretained || check) && residency);
  }
  RetentionPolicy
  policy() const { // as CommandQueue's constructor sets it
    return {.metal4 = metal4, .unretained = unretained, .lifetime_check = check};
  }
  std::string
  name() const {
    return std::string(metal4 ? "metal4" : "metal3") + (unretained ? " unretained" : " retained") +
           (check ? " check" : "") + (residency ? " residency" : "");
  }
};

// The hold call sites (ArgumentEncodingContext::flushCommandsImpl, the present split)
enum class Site {
  PresentSplit, // Metal 3 present split: KeepSplitCommandBuffer
  DrawableM3,   // Metal 3 present
  PresenterM3,
  DrawableM4, // Metal 4 present
  PresenterM4,
  SpatialM3, // MetalFX spatial upscale
  SpatialM4,
  TemporalM3, // MetalFX temporal upscale (Metal 3 only)
  SignalEvent,
  WaitEvent,
};
static const Site kMetal3Sites[] = {Site::PresentSplit, Site::DrawableM3, Site::PresenterM3, Site::SpatialM3,
                                    Site::TemporalM3,   Site::SignalEvent, Site::WaitEvent};
static const Site kMetal4Sites[] = {Site::DrawableM4, Site::PresenterM4, Site::SpatialM4, Site::SignalEvent,
                                    Site::WaitEvent};

// gw2-on-mac 5f16916's gate at each call site: `keep` is gw2::lifetime::keepUntilRetired()
// (DXMT_UNRETAINED=1 or the check); the split and the Metal 4 present held unconditionally.
static bool
legacyGate(Site site, const Config &c) {
  const bool keep = c.unretained || c.check;
  switch (site) {
  case Site::PresentSplit:
  case Site::DrawableM4:
  case Site::PresenterM4:
    return true;
  default:
    return keep;
  }
}

static HoldReason
reasonOf(Site site) {
  switch (site) {
  case Site::PresentSplit: return HoldReason::PresentSplit;
  case Site::DrawableM3:
  case Site::DrawableM4: return HoldReason::Drawable;
  case Site::PresenterM3:
  case Site::PresenterM4: return HoldReason::Presenter;
  case Site::SpatialM3:
  case Site::SpatialM4: return HoldReason::SpatialScaler;
  case Site::TemporalM3: return HoldReason::TemporalScaler;
  case Site::SignalEvent:
  case Site::WaitEvent: return HoldReason::Event;
  }
  return HoldReason::PresentSplit;
}

static std::vector<Config>
allConfigs() {
  std::vector<Config> v;
  for (int bits = 0; bits < 16; bits++)
    v.push_back({bool(bits & 8), bool(bits & 4), bool(bits & 2), bool(bits & 1)});
  return v;
}

// L2: the policy decides as every call site's gate did, in every reachable configuration
static void
testPolicyParity() {
  int reachable = 0;
  for (auto &c : allConfigs()) {
    if (!c.reachable())
      continue;
    reachable++;
    auto policy = c.policy();
    for (Site site : c.metal4 ? std::span<const Site>(kMetal4Sites) : std::span<const Site>(kMetal3Sites)) {
      checks++;
      if (legacyGate(site, c) != policy.holds(reasonOf(site))) {
        failures++;
        std::printf("FAIL policy %s: site %d held %d by the call site, %d by the policy\n", c.name().c_str(),
                    int(site), legacyGate(site, c), policy.holds(reasonOf(site)));
      }
    }
  }
  CHECK(reachable == 11); // 8 Metal 3; 3 Metal 4 (unretained, check or both, with residency)
  // the split is always held; nothing else with Metal 3's retained command buffers and no check
  CHECK(RetentionPolicy{}.holds(HoldReason::PresentSplit));
  CHECK(!RetentionPolicy{}.holds(HoldReason::Drawable));
  CHECK(RetentionPolicy{.metal4 = true}.holds(HoldReason::Event));
}

// gw2-on-mac 74a7836's per-chunk holds, copied: CommandChunk's split_cmdbufs and keep lists, the part
// of reset() that released them, CommandQueue::KeepUntilRetired / KeepSplitCommandBuffer (indexed by
// seq % chunk_count_), and the destructor's reset of all 128 chunks. "reset<i>" stands for the rest of
// reset() (readbacks, command list, allocation tracking, attached_cmdbuf), which ran first. The call
// sites (encode()) with 5f16916's gates.
struct LegacyChunk {
  std::vector<CmdBuf> split_cmdbufs;
  std::vector<uint64_t> keep_objects;
  std::vector<Pres> keep_presenters;
  std::vector<Spatial> keep_spatial_scalers;
  std::vector<Temporal> keep_temporal_scalers;
  void
  reset(uint32_t index) {
    g_released.push_back("reset" + std::to_string(index));
    split_cmdbufs.clear();
    for (auto object : keep_objects)
      TestTraits::release(object);
    keep_objects.clear();
    keep_presenters.clear();
    keep_spatial_scalers.clear();
    keep_temporal_scalers.clear();
  }
};
struct LegacyQueue {
  uint32_t chunk_count_;
  Config config;
  std::array<LegacyChunk, 128> chunks;
  LegacyQueue(uint32_t n, Config c) : chunk_count_(n), config(c) {}
  void
  KeepUntilRetired(uint64_t seq, uint64_t object) {
    if (object)
      chunks[seq % chunk_count_].keep_objects.push_back(object);
  }
  void
  KeepUntilRetired(uint64_t seq, Pres &&p) {
    if (p)
      chunks[seq % chunk_count_].keep_presenters.push_back(std::move(p));
  }
  void
  KeepUntilRetired(uint64_t seq, Spatial &&s) {
    if (s)
      chunks[seq % chunk_count_].keep_spatial_scalers.push_back(std::move(s));
  }
  void
  KeepUntilRetired(uint64_t seq, Temporal &&t) {
    if (t)
      chunks[seq % chunk_count_].keep_temporal_scalers.push_back(std::move(t));
  }
  void
  KeepSplitCommandBuffer(uint64_t seq, CmdBuf &&c) {
    chunks[seq % chunk_count_].split_cmdbufs.push_back(std::move(c));
  }
  // a call site of chunk `seq`; what it doesn't keep goes with its encoder data, right here (a
  // borrowed drawable is never referenced at all)
  void
  encode(uint64_t seq, Site site, const std::string &name) {
    const bool keep = config.unretained || config.check;
    switch (site) {
    case Site::PresentSplit: KeepSplitCommandBuffer(seq, CmdBuf(name)); break;
    case Site::DrawableM4: KeepUntilRetired(seq, object(name)); break;
    case Site::PresenterM4: {
      Pres p(name);
      KeepUntilRetired(seq, std::move(p));
      break;
    }
    case Site::DrawableM3:
      if (keep)
        KeepUntilRetired(seq, object(name));
      break;
    case Site::PresenterM3: {
      Pres p(name);
      if (keep)
        KeepUntilRetired(seq, std::move(p));
      break;
    }
    case Site::SpatialM3:
    case Site::SpatialM4: {
      Spatial sc(name);
      if (keep)
        KeepUntilRetired(seq, std::move(sc));
      break;
    }
    case Site::TemporalM3: {
      Temporal sc(name);
      if (keep)
        KeepUntilRetired(seq, std::move(sc));
      break;
    }
    case Site::SignalEvent:
    case Site::WaitEvent: {
      auto event = object(name); // the encoder data's reference
      if (keep)
        KeepUntilRetired(seq, event);
      else
        TestTraits::release(event); // data->~SignalEventData()
      break;
    }
    }
  }
  std::vector<std::string>
  splitNames(uint64_t seq) {
    std::vector<std::string> v;
    for (auto &c : chunks[seq % chunk_count_].split_cmdbufs)
      v.push_back(c.name);
    return v;
  }
  void
  retire(uint64_t seq) {
    chunks[seq % chunk_count_].reset(seq % chunk_count_);
  }
  void
  teardown() {
    for (unsigned i = 0; i < 128; i++)
      chunks[i].reset(i);
  }
};

// the queue as it is now: CommandQueue::Hold / HoldRetaining / HoldSplit with the policy the
// constructor sets, the chunk's own reset, then the ledger (RetireChunk; the destructor's drain)
struct LedgerQueue {
  uint32_t chunk_count_;
  Ledger ledger_;
  LedgerQueue(uint32_t n, Config c) : chunk_count_(n), ledger_(n, c.policy()) {}
  // CommandQueue::Hold(seq, Reference&&, reason): declined, the caller keeps (and later releases) it
  bool
  Hold(uint64_t seq, uint64_t &object, HoldReason reason) {
    if (!object || !ledger_.policy().holds(reason))
      return false;
    CHECK(ledger_.hold(seq, std::exchange(object, 0), reason));
    return true;
  }
  template <class T>
  void
  Hold(uint64_t seq, T &&owner) {
    CHECK(ledger_.hold(seq, std::move(owner)));
  }
  void
  encode(uint64_t seq, Site site, const std::string &name) {
    switch (site) {
    case Site::PresentSplit: CHECK(ledger_.holdSplit(seq, CmdBuf(name))); break;
    case Site::DrawableM3:
    case Site::DrawableM4:
      // HoldRetaining: a reference is taken only if the policy holds it
      if (ledger_.policy().holds(HoldReason::Drawable)) {
        auto drawable = object(name);
        Hold(seq, drawable, HoldReason::Drawable);
      }
      break;
    case Site::PresenterM3:
    case Site::PresenterM4: {
      Pres p(name);
      Hold(seq, std::move(p));
      break;
    }
    case Site::SpatialM3:
    case Site::SpatialM4: {
      Spatial sc(name);
      Hold(seq, std::move(sc));
      break;
    }
    case Site::TemporalM3: {
      Temporal sc(name);
      Hold(seq, std::move(sc));
      break;
    }
    case Site::SignalEvent:
    case Site::WaitEvent: {
      auto event = object(name);
      if (!Hold(seq, event, HoldReason::Event))
        TestTraits::release(event); // data->~SignalEventData()
      break;
    }
    }
  }
  std::vector<std::string>
  splitNames(uint64_t seq) {
    std::vector<std::string> v;
    for (auto &c : ledger_.splits(seq))
      v.push_back(c.name);
    return v;
  }
  void
  retire(uint64_t seq) {
    g_released.push_back("reset" + std::to_string(seq % chunk_count_));
    CHECK(ledger_.retire(seq));
  }
  void
  teardown() {
    ledger_.drain([](uint32_t slot) { g_released.push_back("reset" + std::to_string(slot)); });
    for (unsigned i = chunk_count_; i < 128; i++)
      g_released.push_back("reset" + std::to_string(i));
  }
};

// One scripted run: chunks encoded with random call sites of the configuration's backend, the finish
// thread retiring behind the ring limit at random lag, and a teardown with chunks still in flight.
// Returns the release trace (with the split buffers the finish thread read before each retirement).
template <class Q>
static std::vector<std::string>
run(uint32_t n, const Config &config, uint32_t seed, uint64_t chunks) {
  std::mt19937 rng(seed);
  // the same names for both queues: the script draws from its own generator
  Q q(n, config);
  auto sites = config.metal4 ? std::span<const Site>(kMetal4Sites) : std::span<const Site>(kMetal3Sites);
  uint64_t retired = 0;
  std::vector<std::string> trace;
  auto flush = [&] {
    for (auto &x : take())
      trace.push_back(x);
  };
  auto retireOne = [&] {
    ++retired;
    for (auto &name : q.splitNames(retired))
      trace.push_back("read:" + name);
    q.retire(retired);
    flush();
  };
  for (uint64_t s = 1; s <= chunks; s++) {
    while (s - retired > n - 1 || (retired < s - 1 && rng() % 4 == 0))
      retireOne();
    auto k = rng() % 6; // how many call sites this chunk passes
    for (uint32_t i = 0; i < k; i++) {
      auto site = sites[rng() % sites.size()];
      q.encode(s, site, "site" + std::to_string(int(site)) + "." + std::to_string(s) + "." + std::to_string(i));
    }
    flush();
  }
  // device teardown: the threads stopped with some chunks unretired
  q.teardown();
  flush();
  return trace;
}

static void
testParity() {
  for (auto &config : allConfigs()) {
    if (!config.reachable())
      continue;
    for (uint32_t n : {32u, 64u, 128u}) {
      for (uint32_t seed = 1; seed <= 10; seed++) {
        g_objects.resize(1);
        auto legacy = run<LegacyQueue>(n, config, seed, 6 * n);
        g_objects.resize(1);
        auto now = run<LedgerQueue>(n, config, seed, 6 * n);
        checks++;
        if (legacy != now) {
          failures++;
          size_t i = 0;
          while (i < legacy.size() && i < now.size() && legacy[i] == now[i])
            i++;
          std::printf("FAIL parity %s ring %u seed %u: traces differ at %zu (legacy %s, ledger %s; lengths %zu, "
                      "%zu)\n",
                      config.name().c_str(), n, seed, i, i < legacy.size() ? legacy[i].c_str() : "<end>",
                      i < now.size() ? now[i].c_str() : "<end>", legacy.size(), now.size());
        }
      }
    }
  }
}

int
main() {
  testNothingBeforeRetire();
  testOrder();
  testNullHolds();
  for (uint32_t n : {32u, 64u, 128u})
    testRingWrap(n);
  testTwoQueues();
  testRefusedRetirements();
  testNoHoldAfterRetirementBegan();
  testDrainOrder();
  testPolicyParity();
  testParity();
  if (failures) {
    std::printf("retirement_ledger_test: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("retirement_ledger_test: all %d checks passed\n", checks);
  return 0;
}
