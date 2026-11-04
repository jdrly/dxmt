// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O1 -Wall -Wextra -Isrc/dxmt tests/gw2/retirement_recovery_test.cpp -o $OUT
// gw2-build: clang++ -std=c++20 -O1 -g -fsanitize=thread -Wall -Wextra -Isrc/dxmt tests/gw2/retirement_recovery_test.cpp -o ${OUT}_tsan
// gw2-run: "$OUT" && TSAN_OPTIONS=halt_on_error=1:exitcode=66 "${OUT}_tsan"
//
// gw2 (#113): native test of the retirement ledger's recovery from a refused retirement
// (BasicRetirementLedger::retireOrRecover in src/dxmt/dxmt_gw2_retirement.hpp, what CommandQueue's
// RetireChunk calls with DXMT_RETIRE_RECOVERY). No Metal, no Wine, no threads.
//
// A scripted queue runs the production discipline (at most ring - 1 chunks in flight, the finish thread
// retiring in sequence order; every chunk holds a drawable, a presenter and a present split, as Metal 4's
// present does) and injects one refusal: an empty token, a missed retirement, a token behind the ledger.
// Every owner is named after its chunk and checks, when released, that a token proving that chunk's
// completion was handed to the ledger first (no early release). The run checks:
// - exactly one refused retirement that doesn't follow another (CommandQueue's error line; the next
//   chunk's catching up follows it, an info line);
// - with recovery, the live holds stay within the ring's worth for the rest of the run, and nothing is
//   left refused; without it (plain retire(), today's behaviour before #113), they pile up until drain;
// - a refusal that lasts past a ring wrap (stale generations refused into the refused holds) is released
//   once their chunks complete, not at drain; a hold refused for a retired sequence waits until every
//   chunk the ring may have in flight completed;
// - a ledger without refusals releases exactly what retire() releases (retireOrRecover's normal path);
// - (the review of #116) threads: an encode thread holding into the ring while the finish thread stalls
//   for more than a ring and recovers, so recovery's scan meets slots the encode thread claims. Run a
//   second time under ThreadSanitizer (${OUT}_tsan): no data race, no early release, nothing left.
//
// Run: tests/gw2/run.zsh pure, or the command above with $OUT set.

#include "dxmt_gw2_retirement.hpp"
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <set>
#include <span>
#include <string>
#include <utility>
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

// --- owners named "<what>:<chunk>[:<last chunk that may use it>]"

static uint64_t g_completed = 0; // the highest chunk a token proved complete so far
static std::set<std::string> g_live;
static std::vector<std::string> g_released;
static int g_early = 0;

static uint64_t
lastUse(const std::string &name) {
  auto colon = name.rfind(':');
  return std::stoull(name.substr(colon + 1));
}

static std::string
named(const char *what, uint64_t seq) {
  std::string name = std::string(what) + ":" + std::to_string(seq);
  g_live.insert(name);
  return name;
}

static void
logRelease(const std::string &name) {
  if (lastUse(name) > g_completed) {
    if (g_early++ < 5)
      std::printf("FAIL: %s released early (completed through %llu)\n", name.c_str(),
                  (unsigned long long)g_completed);
  }
  g_live.erase(name);
  g_released.push_back(name);
}

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
  using CommandBuffer = Owner<0>;
  using Presenter = Owner<1>;
  using SpatialScaler = Owner<2>;
  using TemporalScaler = Owner<3>;
};
using BaseLedger = dxmt::gw2::BasicRetirementLedger<TestTraits>;
using dxmt::gw2::HoldReason;

struct TestIssuer : dxmt::gw2::CompletionIssuer {
  struct Record {};
  dxmt::gw2::BasicCompletedChunk<Record>
  token(uint64_t seq) const {
    return completed(seq, std::span<const Record>{}, dxmt::gw2::ReportOrder::CommitOrder);
  }
  dxmt::gw2::DrainedQueue
  drainedToken() const {
    return drained();
  }
};

struct Ledger : BaseLedger {
  TestIssuer issuer;
  explicit Ledger(uint32_t ring_slots) : BaseLedger(ring_slots, {.metal4 = true}) { acceptTokensFrom(&issuer); }
};

// Metal 4's present: the drawable, the presenter, and a present split (always held)
static void
holdChunk(Ledger &l, uint64_t seq, int &refused) {
  refused += !l.hold(seq, object(named("drawable", seq)), HoldReason::Drawable);
  refused += !l.hold(seq, Owner<1>(named("presenter", seq)));
  refused += !l.holdSplit(seq, Owner<0>(named("split", seq)));
}

enum class Inject { None, EmptyToken, Missed, Behind, LongStall };

struct Run {
  int refused_retirements = 0; // refused retirements; with recovery, those that don't follow a refused one
                               // (CommandQueue's error lines)
  int refused_holds = 0;
  size_t peak_live_after = 0;  // the most live holds from a ring after the recovery to the end
  size_t refused_left = 0;     // ledger.refusedHolds() before drain
  size_t live_before_drain = 0;
};

// chunks 1..total; the finish thread retires chunk `seq` with retireOrRecover (or plain retire()); the
// injection hits chunk `at`
static Run
run(uint32_t n, uint64_t total, Inject inject, uint64_t at, bool recover) {
  Run r;
  g_completed = 0;
  g_live.clear();
  g_objects.resize(1);
  g_early = 0;
  {
    Ledger l(n);
    uint64_t finished = 0; // the finish thread's position (every chunk up to it waited for)
    auto finishOne = [&] {
      uint64_t seq = ++finished;
      g_completed = seq; // the submission's wait(seq) returned: chunk seq (and every earlier one) completed
      auto token = l.issuer.token(seq);
      bool stalled = inject == Inject::LongStall && seq >= at && seq < at + 2 * n;
      if ((inject == Inject::EmptyToken && seq == at) || stalled) {
        auto taken = std::move(token); // the injected bug: the token was moved out before the retirement
      }
      if (inject == Inject::Missed && seq == at)
        return; // the injected bug: the finish thread skips this chunk's retirement
      if (inject == Inject::Behind && seq == at)
        token = l.issuer.token(seq - 1); // the injected bug: the previous chunk's token again
      if (recover) {
        auto outcome = l.retireOrRecover(token);
        dxmt::gw2::RetireStep step{.seq = seq, .ledger_refused = !outcome.retired, .ledger = outcome};
        r.refused_retirements += dxmt::gw2::retireDiagnostic(step).line == dxmt::gw2::RetireLine::Error;
        CHECK(outcome.after == l.retired());
      } else {
        r.refused_retirements += !l.retire(token);
      }
    };
    for (uint64_t s = 1; s <= total; s++) {
      while (s - finished > n - 1)
        finishOne();
      holdChunk(l, s, r.refused_holds);
      // from a ring after the recovery (a long stall's recovery comes once it ends, ring - 1 chunks later)
      if (s > at + (inject == Inject::LongStall ? 4 : 2) * n + 2)
        r.peak_live_after = std::max(r.peak_live_after, g_live.size());
    }
    while (finished < total)
      finishOne();
    r.refused_left = l.refusedHolds();
    r.live_before_drain = g_live.size();
    CHECK(l.drain(l.issuer.drainedToken()));
    CHECK(g_live.empty());
  }
  CHECK(g_live.empty());
  CHECK(g_early == 0);
  return r;
}

static void
testOneRefusal() {
  for (uint32_t n : {32u, 64u, 128u}) {
    uint64_t total = 10 * n, at = 3 * n + 5;
    // ring - 1 chunks in flight, 3 holds each, and the chunk being encoded
    size_t bound = size_t(n) * 3;
    for (Inject inject : {Inject::EmptyToken, Inject::Missed, Inject::Behind}) {
      auto with = run(n, total, inject, at, true);
      CHECK(with.refused_retirements == 1);
      CHECK(with.refused_holds == 0);
      CHECK(with.peak_live_after <= bound);
      CHECK(with.refused_left == 0);
      CHECK(with.live_before_drain == 0); // every chunk retired by the end
      // without recovery (before #113): every later retirement refused, the holds pile up until drain
      auto without = run(n, total, inject, at, false);
      CHECK(without.refused_retirements > int(total - at - 2));
      CHECK(without.live_before_drain > (total - at - 1) * 3 - 3);
      CHECK(without.peak_live_after > bound);
    }
  }
}

// one bug that refuses 2 rings' worth of retirements (empty tokens): the ring wraps, later chunks' holds
// are refused as stale generations; the first good token recovers, and the refused holds go as their
// chunks complete
static void
testLongStall() {
  for (uint32_t n : {32u, 64u}) {
    uint64_t total = 10 * n, at = 2 * n;
    auto r = run(n, total, Inject::LongStall, at, true);
    CHECK(r.refused_retirements == 1);
    CHECK(r.refused_holds > 0);
    CHECK(r.peak_live_after <= size_t(n) * 3);
    CHECK(r.refused_left == 0);
    CHECK(r.live_before_drain == 0);
  }
}

// a hold for a retired sequence (a late hold): kept until every chunk the ring may have in flight
// completed (the last retired + ring - 1), released then, not at drain
static void
testLateHold() {
  const uint32_t n = 8;
  g_completed = 0;
  g_live.clear();
  g_released.clear();
  Ledger l(n);
  auto retire = [&](uint64_t seq) {
    g_completed = seq;
    auto outcome = l.retireOrRecover(l.issuer.token(seq));
    CHECK(outcome.retired);
    return outcome;
  };
  CHECK(l.hold(1, Owner<1>(named("presenter", 1))));
  retire(1);
  // late: chunk 1 retired; any chunk up to 1 + 8 - 1 = 8 may be in flight
  CHECK(!l.hold(1, Owner<1>("late:1:8")));
  g_live.insert("late:1:8");
  CHECK(l.refusedHolds() == 1);
  for (uint64_t s = 2; s <= 7; s++) {
    retire(s);
    CHECK(l.refusedHolds() == 1);
  }
  auto outcome = retire(8);
  CHECK(outcome.refused_holds == 1);
  CHECK(l.refusedHolds() == 0);
  CHECK(!g_live.count("late:1:8"));
  CHECK(g_early == 0);
  CHECK(l.drain(l.issuer.drainedToken()));
}

// without refusals, retireOrRecover releases what retire() releases, when it does
static void
testNormalPath() {
  for (uint32_t n : {32u, 64u}) {
    std::vector<std::string> traces[2];
    for (int recover = 0; recover < 2; recover++) {
      g_released.clear();
      run(n, 6 * n, Inject::None, ~0ull / 2, recover);
      traces[recover] = g_released;
    }
    CHECK(!traces[0].empty() && traces[0] == traces[1]);
  }
}

// a refused token of another issuer, or an empty one, recovers nothing by itself; after drain nothing
static void
testUnauthorized() {
  g_completed = 0;
  g_live.clear();
  Ledger l(8);
  TestIssuer other;
  CHECK(l.hold(1, Owner<1>(named("presenter", 1))));
  auto outcome = l.retireOrRecover(other.token(1));
  CHECK(!outcome.retired && !outcome.recovered && outcome.after == 0 && outcome.chunks == 0);
  CHECK(g_live.count("presenter:1"));
  g_completed = 1;
  CHECK(l.retireOrRecover(l.issuer.token(1)).retired);
  CHECK(!g_live.count("presenter:1"));
  CHECK(l.drain(l.issuer.drainedToken()));
  outcome = l.retireOrRecover(l.issuer.token(2));
  CHECK(!outcome.retired && !outcome.recovered);
  CHECK(g_early == 0);
}

// --- threads (the review of #116): owners counted atomically; a release before its chunk's completion
// was proved is early

static std::atomic<uint64_t> t_completed{0};
static std::atomic<int64_t> t_live{0};
static std::atomic<int> t_early{0};

static void
threadRelease(uint64_t seq) {
  if (seq > t_completed.load(std::memory_order_acquire))
    t_early.fetch_add(1);
  t_live.fetch_sub(1);
}

struct ThreadPresenter {
  uint64_t seq = 0;
  ThreadPresenter() = default;
  explicit ThreadPresenter(uint64_t s) : seq(s) { t_live.fetch_add(1); }
  ThreadPresenter(ThreadPresenter &&o) noexcept : seq(std::exchange(o.seq, 0)) {}
  ThreadPresenter &
  operator=(ThreadPresenter &&o) noexcept {
    reset();
    seq = std::exchange(o.seq, 0);
    return *this;
  }
  ~ThreadPresenter() { reset(); }
  void
  reset() {
    if (seq)
      threadRelease(std::exchange(seq, 0));
  }
  explicit operator bool() const { return seq != 0; }
};

struct ThreadTraits {
  using Object = uint64_t; // the chunk's sequence
  static void
  release(uint64_t seq) {
    threadRelease(seq);
  }
  using CommandBuffer = std::unique_ptr<char>;
  using Presenter = ThreadPresenter;
  using SpatialScaler = std::unique_ptr<short>;
  using TemporalScaler = std::unique_ptr<long>;
};

static void
testThreads() {
  const uint32_t n = 8;
  // a stall of 3 rings; it ends mid-ring, so the first chunk to claim index n - 1 after it (stall_to +
  // n/2 - 1) is within the n - 2 chunks the encode thread may run ahead of the recovery at stall_to
  const uint64_t total = 40 * n, stall_from = 5 * n, stall_to = 8 * n + n / 2;
  for (int round = 0; round < 20; round++) {
    t_completed = 0;
    t_live = 0;
    t_early = 0;
    std::atomic<uint64_t> committed{0}, finished{0};
    size_t refused_left = 0;
    int error_lines = 0;
    {
      dxmt::gw2::BasicRetirementLedger<ThreadTraits> l(n, {.metal4 = true});
      TestIssuer issuer;
      l.acceptTokensFrom(&issuer);
      std::thread encode([&] {
        for (uint64_t s = 1; s <= total; s++) {
          // the ring discipline: at most n - 1 chunks in flight (CommitCurrentChunk)
          while (s - finished.load(std::memory_order_acquire) > n - 1)
            std::this_thread::yield();
          // the review's case: the ring slot of index n - 1 holds nothing until the stall ends, so it is
          // still free when recovery scans it, and the encode thread claims it for a chunk up to ring - 2
          // ahead of the recovery (no handshake orders the two); every fifth chunk holds nothing either
          bool free_slot = s % n == n - 1 && s < stall_to;
          if (!free_slot && s % 5) {
            t_live.fetch_add(1);
            (void)l.hold(s, uint64_t(s), HoldReason::Drawable);
            (void)l.hold(s, ThreadPresenter(s));
          }
          committed.store(s, std::memory_order_release); // ready_for_commit
        }
      });
      for (uint64_t seq = 1; seq <= total; seq++) {
        while (committed.load(std::memory_order_acquire) < seq)
          std::this_thread::yield();
        t_completed.store(seq, std::memory_order_release); // the submission's wait(seq) returned
        auto token = issuer.token(seq);
        if (seq >= stall_from && seq < stall_to)
          auto taken = std::move(token); // the injected bug: 3 rings of empty tokens
        auto outcome = l.retireOrRecover(token);
        dxmt::gw2::RetireStep step{.seq = seq, .ledger_refused = !outcome.retired, .ledger = outcome};
        error_lines += dxmt::gw2::retireDiagnostic(step).line == dxmt::gw2::RetireLine::Error;
        finished.store(seq, std::memory_order_release); // chunk_ongoing
      }
      encode.join();
      refused_left = l.refusedHolds();
      CHECK(l.retired() == total);
      CHECK(l.drain(issuer.drainedToken()));
    }
    CHECK(error_lines == 1);
    CHECK(refused_left == 0);
    CHECK(t_live.load() == 0);
    CHECK(t_early.load() == 0);
  }
}

// RetireChunk's line decision (gw2::retireDiagnostic, the review of #116): one line per step, naming both
// refusals of an empty token; an error for a new failure, info for one that follows it; two failures in a
// row, each recovered, are two errors
static void
testDiagnostic() {
  using dxmt::gw2::RetireLine;
  using dxmt::gw2::RetireStep;
  auto line = [](const RetireStep &s) { return dxmt::gw2::retireDiagnostic(s); };
  CHECK(line(RetireStep{.seq = 5}).line == RetireLine::None);
  RetireStep empty{.seq = 5, .ledger_refused = true, .submission_refused = true, .submission_recovered = true};
  auto d = line(empty);
  CHECK(d.line == RetireLine::Error && d.ledger_part && d.submission_part);
  empty.ledger.follows = true;
  CHECK(line(empty).line == RetireLine::Info);
  auto sub = line(RetireStep{.seq = 5, .submission_refused = true});
  CHECK(sub.line == RetireLine::Error && !sub.ledger_part && sub.submission_part);
  // empty tokens at chunks 3 and 5 of a ledger: 3 errors (one line), 4 catches up (info), 5 errors again
  g_completed = 0;
  g_live.clear();
  Ledger l(8);
  int errors = 0, infos = 0;
  for (uint64_t seq = 1; seq <= 7; seq++) {
    g_completed = seq;
    auto token = l.issuer.token(seq);
    if (seq == 3 || seq == 5)
      auto taken = std::move(token);
    auto outcome = l.retireOrRecover(token);
    // the submission refuses the same empty token (and recovers with retireWaited)
    bool refused = seq == 3 || seq == 5;
    auto diag = line(RetireStep{.seq = seq,
                                .ledger_refused = !outcome.retired,
                                .ledger = outcome,
                                .submission_refused = refused,
                                .submission_recovered = refused});
    errors += diag.line == RetireLine::Error;
    infos += diag.line == RetireLine::Info;
    CHECK(diag.line == (refused ? RetireLine::Error : seq == 4 || seq == 6 ? RetireLine::Info : RetireLine::None));
  }
  CHECK(errors == 2 && infos == 2 && l.retired() == 7);
  CHECK(l.drain(l.issuer.drainedToken()));
}

int
main() {
  testOneRefusal();
  testLongStall();
  testLateHold();
  testNormalPath();
  testUnauthorized();
  testDiagnostic();
  testThreads();
  if (failures) {
    std::printf("retirement_recovery_test: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("retirement_recovery_test: all %d checks passed\n", checks);
  return 0;
}
