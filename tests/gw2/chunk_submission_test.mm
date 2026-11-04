// gw2-test: metal
// gw2-build: U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
// gw2-build: (( ${#U} >= 5 ))
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   for f in $U; do clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Wno-unused-function -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $T/u_${f:r}_$arch.o; done
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal src/winemetal/winemetal_thunks.c -o $T/thunks_$arch.o
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c -o $T/airconv_absent_$arch.o
// gw2-build:   clang++ -std=c++20 -fobjc-arc -Wall -Wextra -arch $arch -mmacosx-version-min=26.0 -Itests/gw2/shim -Isrc/winemetal -Isrc/dxmt tests/gw2/chunk_submission_test.mm src/dxmt/dxmt_gw2_submission.cpp src/dxmt/dxmt_gw2_mtl4_queue.cpp $T/thunks_$arch.o $T/u_*_$arch.o $T/airconv_absent_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -o ${OUT}_$arch
// gw2-build: done
// gw2-run: ${OUT}_x86_64 && MTL_DEBUG_LAYER=1 ${OUT}_arm64
//
// gw2 (#78, Q2, architecture council §2): native test of chunk submission
// (src/dxmt/dxmt_gw2_submission.cpp) on real Metal, against both production adapters, Metal3Submission
// and Metal4Submission (over gw2::Mtl4Queue), through the real bridge: the PE side
// (winemetal_thunks.c) and the real unix side linked natively with DXMT_NATIVE, as
// tests/gw2/mtl4_queue_test.mm does. No Wine, no game.
//
// The test drives each adapter as CommandQueue does (CommitChunkInternal on an encode thread,
// WaitForFinishThread on a finish thread, the ring discipline of at most ring - 1 chunks in flight): a
// chunk begins, may wait for an event, is encoded by a ChunkEncoder that does what flushCommands does
// (GPU work, the present split's early commits, the chunk's completion signal), commits; the finish
// thread waits, reports it (gw2::reportCompletedChunk), resets "the chunk" (drops its last command
// buffer and the split buffers it held) and retires it. What it checks, per adapter:
// - each adapter hands the encoder its own target (Metal 3 a command buffer, Metal 4 the queue);
// - an empty chunk (Metal 3: one command buffer, one record; Metal 4: no command buffer, no record, its
//   completion signal alone), and chunks with 0, 1 and 3 splits give that many + 1 records, in commit
//   order, completed, without an error or logs, and the GPU did each chunk's work once wait() returns;
// - the residency hook runs before every commit (Metal 3: the adapter's commit, while the chunk's last
//   buffer is still uncommitted; Metal 4, gw2 #86: in Mtl4Queue::commit before each commit is submitted,
//   while the open Mtl4Commit has no queue value yet, once per commit, the encoding's and commit()'s);
// - timing: Metal 3 reads GPU times only when asked; Metal 4's feedback always has them;
// - the report's order (Metal 3: the last buffer first) and its spans (one per record);
// - waitEvent: a chunk that waits for an event the CPU signals isn't complete before the signal;
// - Metal 4's wait() returns only once the chunk's completion event fired (an empty chunk that waits
//   for an event: no commit to wait for, only that event);
// - drain() with chunks committed and never waited for: every one of them completed afterwards.
// - gw2 (#91, L3): the tokens. A retirement ledger (the production template, holding the present
//   split's buffers as CommandQueue's does) is bound to the adapter and retired with each wait()'s token,
//   then drained with drain()'s; another issuer's token, for the same chunk, is refused by the ledger
//   and by the adapter's retire() and changes nothing; so is an empty (moved-from) one.
// - with the deterministic test completion adapter (ScriptedSubmission, no GPU): error records and
//   the throttle, missing GPU times reported as 0, the token's records unchanged until retire, wait and
//   retire in order, drain() with chunks committed and never waited for, the same token checks, and
//   (the review of #97) a stale token of the adapter's own, from an earlier wait, refused and kept.
// No deliberate GPU faults (council §2); the parity of the records with the code they replaced is
// tests/gw2/commit_record_test.cpp's.
// Exit 77 without a Metal device; without Metal 4, the Metal 3 half runs and the test exits 77
// (skipped, never passed).

#import <Metal/Metal.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "dxmt_gw2_mtl4_queue.hpp"
#include "dxmt_gw2_retirement.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_submission.hpp"
#include <memory>
extern "C" {
#include "winemetal_thunks.h"
}

using namespace dxmt;
using dxmt::gw2::Mtl4Commit;
using dxmt::gw2::Mtl4Queue;

static std::atomic<int> failures{0};
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                  \
      std::printf(__VA_ARGS__);                                                                    \
      std::printf("\n");                                                                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// --- what DXMT's statistics and residency modules provide
static std::atomic<int> g_before_commits{0};
static std::atomic<obj_handle_t> g_uncommitted{0}; // Metal 3: the buffer the adapter is about to commit
static std::atomic<int> g_committed_too_early{0};
// gw2 (#86): Metal 4: the commit Mtl4Queue::commit is about to submit, and the commits whose hook ran
// while their queue value was still 0 (before `commit->value = ++next_value_` and the queue's commit)
static std::atomic<Mtl4Commit *> g_open_mtl4{nullptr};
static std::atomic<int> g_mtl4_hooked_before{0};
namespace dxmt::gw2 {
void
registerSection(const char *, SectionFn) {}
bool
statsEnabled() {
  return true;
}
// gw2 (#29): the device set's pending changes, before every commit
void
residencyBeforeCommit() {
  g_before_commits++;
  if (auto h = g_uncommitted.exchange(0))
    if (WMT::CommandBuffer{h}.status() != WMTCommandBufferStatusNotEnqueued)
      g_committed_too_early++;
  if (auto *commit = g_open_mtl4.exchange(nullptr)) {
    if (commit->value != 0)
      g_committed_too_early++;
    else
      g_mtl4_hooked_before++;
  }
}
} // namespace dxmt::gw2

#define OBJ(h) ((__bridge id)(void *)(uintptr_t)(h))
#define HANDLE(o) ((obj_handle_t)(uintptr_t)(__bridge void *)(o))

constexpr uint32_t kRing = 4;

// one chunk's shape
struct Shape {
  const char *name;
  bool work;      // GPU work in each command buffer (Metal 4: the chunk has no command buffer without it)
  int splits;     // early commits of the present split
  bool timing;    // DXMT_STATS
  bool external;  // waits for the CPU-signalled event
  bool leave_open; // Metal 4: the encoding leaves its last command buffer open for commit()
};
static const Shape kShapes[] = {
    {"empty", false, 0, true, false, false},       {"0 splits", true, 0, true, false, false},
    {"1 split", true, 1, true, false, true},       {"3 splits", true, 3, true, false, false},
    {"3 splits, no timing", true, 3, false, false, true}, {"event wait", true, 1, true, true, false},
    // Metal 4: no command buffer, so only the completion event says the chunk (its queue wait) is done
    {"empty, event wait", false, 0, true, true, false},
};
constexpr uint64_t kChunks = 3 * (sizeof(kShapes) / sizeof(kShapes[0]));

static const Shape &
shapeOf(uint64_t seq) {
  return kShapes[(seq - 1) % (sizeof(kShapes) / sizeof(kShapes[0]))];
}

// the GPU work: the chunk's byte at (seq % ring) * 4 + index of `fill` = seq
static void
fillMetal3(WMT::CommandBuffer cmdbuf, id<MTLBuffer> fill, uint64_t seq, uint32_t index) {
  auto encoder = cmdbuf.blitCommandEncoder();
  wmtcmd_blit_fillbuffer cmd{};
  cmd.type = WMTBlitCommandFillBuffer;
  cmd.next.set(nullptr);
  cmd.buffer = HANDLE(fill);
  cmd.offset = (seq % kRing) * 4 + index;
  cmd.length = 1;
  cmd.value = uint8_t(seq);
  MTLBlitCommandEncoder_encodeCommands(encoder, (const struct wmtcmd_base *)&cmd);
  encoder.endEncoding();
}

static void
fillMetal4(Mtl4Queue &queue, Mtl4Commit &commit, id<MTLBuffer> fill, uint64_t seq, uint32_t index) {
  WMTMtl4BindingState state{};
  state.residency_set = queue.residency(seq).handle;
  auto encoder = WMT::MTL4CommandBuffer{commit.cmdbuf.handle}.computeCommandEncoder(&state);
  wmtcmd_blit_fillbuffer cmd{};
  cmd.type = WMTBlitCommandFillBuffer;
  cmd.next.set(nullptr);
  cmd.buffer = HANDLE(fill);
  cmd.offset = (seq % kRing) * 4 + index;
  cmd.length = 1;
  cmd.value = uint8_t(seq);
  encoder.encodeBlitCommands((const wmtcmd_blit_nop *)&cmd);
  encoder.endEncoding();
}

// gw2 (#91): the retirement ledger as production instantiates it (RetirementTraits in
// dxmt_command_queue.hpp), with owners for the kinds this test doesn't hold
struct LedgerTraits {
  using Object = obj_handle_t;
  static void
  release(obj_handle_t object) {
    WMT::Object{object}.release();
  }
  using CommandBuffer = WMT::Reference<WMT::CommandBuffer>;
  using Presenter = std::unique_ptr<int>; // distinct types, as the hold overloads need
  using SpatialScaler = std::unique_ptr<short>;
  using TemporalScaler = std::unique_ptr<long>;
};
using Ledger = gw2::BasicRetirementLedger<LedgerTraits>;

// gw2 (#91): tokens from another issuer (another queue's submission)
struct ForeignIssuer : gw2::CompletionIssuer {
  CompletedChunk
  token(uint64_t seq) const {
    return completed(seq, std::span<const CommitRecord>{}, gw2::ReportOrder::CommitOrder);
  }
  gw2::DrainedQueue
  drainedToken() const {
    return drained();
  }
};

// What CommandQueue keeps per ring slot: the chunk's last command buffer (Metal 3; released when the
// chunk resets); the split buffers are the retirement ledger's (gw2 #66, #91)
struct Chunks {
  WMT::Reference<WMT::CommandBuffer> last[kRing];
  Ledger ledger{kRing, {.unretained = true}};
};

// flushCommands for the test: the chunk's work, its present split's early commits
struct Encoder final : ChunkEncoder {
  WMT::CommandQueue metal3_queue;
  Chunks &chunks;
  bool ordered_commits = true; // gw2 (#86): Metal 4: every commit's hook ran before it was submitted
  id<MTLBuffer> fill;
  uint64_t seq;
  const Shape &shape;
  int metal3_calls = 0, metal4_calls = 0;

  Encoder(WMT::CommandQueue q, Chunks &c, id<MTLBuffer> f, uint64_t s, const Shape &sh) :
      metal3_queue(q),
      chunks(c),
      fill(f),
      seq(s),
      shape(sh) {}

  void
  encode(WMT::CommandBuffer &cmdbuf) override {
    metal3_calls++;
    auto slot = seq % kRing;
    chunks.last[slot] = cmdbuf;
    for (int i = 0; i < shape.splits; i++) {
      if (shape.work)
        fillMetal3(cmdbuf, fill, seq, i);
      gw2::residencyBeforeCommit();
      cmdbuf.commit();
      CHECK(chunks.ledger.holdSplit(seq, cmdbuf), "chunk %llu: split hold refused", (unsigned long long)seq); // CommandQueue::HoldSplit
      cmdbuf = metal3_queue.commandBuffer();     // CommandQueue::CreateCommandBuffer
    }
    if (shape.work)
      fillMetal3(cmdbuf, fill, seq, shape.splits);
    chunks.last[slot] = cmdbuf; // CommandChunk::encode
  }

  void
  encode(Mtl4Queue &queue) override {
    metal4_calls++;
    if (!shape.work)
      return; // no encoder, no command buffer
    for (int i = 0; i <= shape.splits; i++) {
      auto &commit = queue.begin(seq);
      fillMetal4(queue, commit, fill, seq, i);
      if (i < shape.splits || !shape.leave_open) {
        // the present split; or the encoding's own signal commits the rest
        ordered_commits &= commitMetal4(queue, seq);
      }
    }
  }

  // gw2 (#86): Mtl4Queue::commit with the residency hook watching the open commit: true if there was
  // none, or if the hook ran (with the commit's queue value still 0) and the commit was then submitted
  static bool
  commitMetal4(Mtl4Queue &queue, uint64_t seq) {
    auto *open = queue.current(seq);
    g_open_mtl4 = open;
    queue.commit(seq);
    bool hooked = g_open_mtl4.exchange(nullptr) == nullptr; // the hook took it
    return !open || (hooked && open->value != 0);
  }
};

struct Sink {
  int lines = 0;
  size_t spans = 0;
  int aftergpu = 0;
  std::chrono::steady_clock::time_point
  now() {
    return std::chrono::steady_clock::now();
  }
  void
  deviceError(const CommitRecord &) {
    lines++;
  }
  void
  lastLog(const CommitRecord &, const std::string &) {
    lines++;
  }
  void
  splitError(const CommitRecord &, size_t, size_t, uint64_t, uint64_t) {
    lines++;
  }
  void
  gpuSpan(uint64_t, uint64_t) {
    spans++;
  }
  void
  presentGpuEnd(uint64_t) {
    aftergpu++;
  }
};

// Runs kChunks chunks through `sub` with an encode thread (this one) and a finish thread, as
// CommandQueue does, then two chunks that are never waited for, then drain().
static void
run(const char *backend, Mtl4Queue *mtl4, ChunkSubmission &sub, WMT::CommandQueue metal3_queue, Chunks &chunks,
    id<MTLDevice> device, WMT::SharedEvent completion) {
  bool metal4 = mtl4 != nullptr;
  chunks.ledger.setPolicy({.metal4 = metal4, .unretained = true});
  chunks.ledger.acceptTokensFrom(&sub); // gw2 (#91): as CommandQueue's constructor
  ForeignIssuer foreign;
  int hooked_before_start = g_mtl4_hooked_before.load();
  id<MTLBuffer> fill = [device newBufferWithLength:4 * kRing
                                           options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
  id<MTLSharedEvent> external = [device newSharedEvent];
  std::atomic<uint64_t> committed{0}, retired{0};
  uint64_t completion_value = completion.signaledValue();
  std::vector<uint64_t> values(kChunks + 3);
  int before_commits_start = g_before_commits.load();
  std::atomic<int> commits_seen{0};
  int metal3_buffers = 0;

  std::thread finish([&] {
    for (uint64_t seq = 1; seq <= kChunks; seq++) {
      while (committed.load(std::memory_order_acquire) < seq)
        std::this_thread::yield();
      auto &shape = shapeOf(seq);
      auto completed = sub.wait(seq, values[seq], shape.timing);
      auto records = completed.records();
      size_t want = metal4 ? (shape.work ? shape.splits + 1 : 0) : shape.splits + 1;
      CHECK(completed.seq() == seq, "%s %s: token for chunk %llu", backend, shape.name, (unsigned long long)completed.seq());
      CHECK(records.size() == want, "%s %s: %zu records, want %zu", backend, shape.name, records.size(), want);
      CHECK(completed.order() == (metal4 ? gw2::ReportOrder::CommitOrder : gw2::ReportOrder::LastFirst),
            "%s: report order", backend);
      for (size_t i = 0; i < records.size(); i++) {
        auto &r = records[i];
        CHECK(r.ordinal == i, "%s %s: record %zu has ordinal %u", backend, shape.name, i, r.ordinal);
        CHECK(!r.error && r.logs.empty(), "%s %s: record %zu has an error or logs", backend, shape.name, i);
        bool want_times = metal4 || shape.timing;
        CHECK(bool(r.gpu_start_ns) == want_times && bool(r.gpu_end_ns) == want_times,
              "%s %s: record %zu: GPU times %s", backend, shape.name, i, want_times ? "missing" : "read without timing");
        if (r.gpu_start_ns && r.gpu_end_ns)
          CHECK(*r.gpu_end_ns >= *r.gpu_start_ns, "%s %s: record %zu ends before it starts", backend, shape.name, i);
      }
      if (metal4)
        CHECK(completion.signaledValue() >= values[seq], "%s %s: wait() returned before the completion event",
              backend, shape.name);
      if (shape.work) {
        auto *bytes = (const uint8_t *)fill.contents + (seq % kRing) * 4;
        for (int i = 0; i <= shape.splits; i++)
          CHECK(bytes[i] == uint8_t(seq), "%s %s: the GPU work of command buffer %d isn't done", backend, shape.name, i);
      }
      commits_seen += int(records.size());
      Sink sink;
      gw2::BasicSplitErrorThrottle<std::chrono::steady_clock::time_point> throttle;
      gw2::reportCompletedChunk(records, completed.order(), shape.timing, throttle, sink);
      CHECK(sink.lines == 0 && sink.spans == (shape.timing ? records.size() : 0) &&
                sink.aftergpu == (shape.timing && !records.empty() ? 1 : 0),
            "%s %s: report: %d lines, %zu spans, %d aftergpu", backend, shape.name, sink.lines, sink.spans, sink.aftergpu);
      // CommandQueue::RetireChunk: the chunk's reset, its holds (gw2 #91: with the token), then the
      // submission. Another queue's token for the same chunk first: refused by both, nothing changes.
      chunks.last[seq % kRing] = nullptr;
      size_t splits_held = chunks.ledger.splits(seq).size();
      CHECK(!chunks.ledger.retire(foreign.token(seq)), "%s: the ledger took another issuer's token", backend);
      CHECK(!sub.retire(foreign.token(seq)), "%s: retire() took another issuer's token", backend);
      CHECK(chunks.ledger.retired() == seq - 1 && chunks.ledger.splits(seq).size() == splits_held &&
                splits_held == size_t(metal4 ? 0 : shape.splits), // Metal 4's split commits are Mtl4Queue's
            "%s %s: a refused token changed the ledger", backend, shape.name);
      CHECK(completed.issuer() == &sub && sub.issued(completed), "%s: the token isn't the adapter's", backend);
      CHECK(chunks.ledger.retire(completed), "%s %s: the ledger refused the adapter's token", backend, shape.name);
      CHECK(chunks.ledger.splits(seq).empty(), "%s %s: the ledger kept the split buffers", backend, shape.name);
      CHECK(sub.retire(std::move(completed)), "%s %s: retire() refused its own token", backend, shape.name);
      CHECK(!completed && !sub.retire(std::move(completed)), "%s: an empty token was taken back", backend);
      retired.store(seq, std::memory_order_release);
    }
  });

  auto commitChunk = [&](uint64_t seq) {
    auto &shape = shapeOf(seq);
    @autoreleasepool {
      values[seq] = ++completion_value;
      sub.begin(seq);
      if (shape.external)
        sub.waitEvent(WMT::Event{HANDLE(external)}, seq);
      Encoder encoder(metal3_queue, chunks, fill, seq, shape);
      sub.encode(seq, encoder);
      CHECK(encoder.metal3_calls == (metal4 ? 0 : 1) && encoder.metal4_calls == (metal4 ? 1 : 0),
            "%s: the encoder got the wrong backend's target", backend);
      // flushCommands' end: the chunk's completion signal (Metal 3: in its last buffer; Metal 4 at the
      // queue, after what the encoding committed)
      sub.signalEvent(completion, values[seq]);
      if (!metal4) {
        g_uncommitted = chunks.last[seq % kRing].handle;
        metal3_buffers += shape.splits + 1;
      }
      // gw2 (#86): Metal 4: commit() submits the commit left open, if any; its hook first
      auto *open = metal4 ? mtl4->current(seq) : nullptr;
      g_open_mtl4 = open;
      sub.commit(seq);
      bool hooked = g_open_mtl4.exchange(nullptr) == nullptr;
      if (!metal4)
        CHECK(g_uncommitted.exchange(0) == 0, "%s %s: commit() didn't run the residency hook first", backend, shape.name);
      else
        CHECK(!open || (hooked && open->value != 0), "%s %s: commit() didn't run the residency hook before it submitted",
              backend, shape.name);
      CHECK(encoder.ordered_commits, "%s %s: an encoding commit didn't run the residency hook before it was submitted",
            backend, shape.name);
    }
    committed.store(seq, std::memory_order_release);
  };

  for (uint64_t seq = 1; seq <= kChunks; seq++) {
    while (seq > retired.load(std::memory_order_acquire) + (kRing - 1))
      std::this_thread::yield();
    commitChunk(seq);
    if (shapeOf(seq).external) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      CHECK(retired.load() < seq, "%s: chunk %llu completed before the event it waits for", backend,
            (unsigned long long)seq);
      external.signaledValue = seq;
    }
  }
  finish.join();
  CHECK(g_committed_too_early == 0, "%s: the residency hook ran after a commit", backend);
  int before = g_before_commits.load() - before_commits_start;
  // Metal 3: the test's split commits call it, and the adapter's commit once per chunk; Metal 4:
  // Mtl4Queue::commit, once per commit
  CHECK(before == (metal4 ? commits_seen.load() : metal3_buffers), "%s: %d residency commits for %d commits", backend,
        before, metal4 ? commits_seen.load() : metal3_buffers);
  // gw2 (#86): Metal 4: every commit was seen by the hook before the queue value was assigned
  if (metal4)
    CHECK(g_mtl4_hooked_before.load() - hooked_before_start == commits_seen.load(),
          "%s: the hook ran before %d of %d commits were submitted", backend,
          g_mtl4_hooked_before.load() - hooked_before_start, commits_seen.load());

  // teardown: two chunks committed, never waited for (the finish thread stopped), then drain()
  for (uint64_t seq = kChunks + 1; seq <= kChunks + 2; seq++)
    commitChunk(seq);
  // gw2 (#91): CommandQueue::DrainChunks: the submission's drain, then the ledger's with its token
  // (another issuer's is refused first and releases nothing)
  auto drained = sub.drain();
  CHECK(drained && drained.issuer() == &sub, "%s: drain() gave no token of its own", backend);
  CHECK(!chunks.ledger.drain(foreign.drainedToken()), "%s: the ledger drained with another issuer's token", backend);
  size_t pending_splits = 0;
  for (uint64_t seq = kChunks + 1; seq <= kChunks + 2; seq++)
    pending_splits += chunks.ledger.splits(seq).size();
  CHECK(pending_splits == (metal4 ? 0 : size_t(shapeOf(kChunks + 1).splits + shapeOf(kChunks + 2).splits)),
        "%s: a refused drain released split buffers", backend);
  for (uint64_t seq = kChunks + 1; seq <= kChunks + 2; seq++) {
    auto &shape = shapeOf(seq);
    if (!metal4)
      CHECK(chunks.last[seq % kRing].status() == WMTCommandBufferStatusCompleted,
            "%s: drain() returned before chunk %llu completed", backend, (unsigned long long)seq);
    if (shape.work) {
      auto *bytes = (const uint8_t *)fill.contents + (seq % kRing) * 4;
      CHECK(bytes[shape.splits] == uint8_t(seq), "%s: drain() returned before chunk %llu's work", backend,
            (unsigned long long)seq);
    }
    chunks.last[seq % kRing] = nullptr;
  }
  CHECK(chunks.ledger.drain(drained), "%s: the ledger refused drain()'s token", backend);
  for (uint64_t seq = kChunks + 1; seq <= kChunks + 2; seq++)
    CHECK(chunks.ledger.splits(seq).empty(), "%s: the drain kept chunk %llu's split buffers", backend,
          (unsigned long long)seq);
  std::printf("%s %s: %llu chunks in a ring of %u (%zu shapes), then 2 drained\n", failures ? "...." : "ok  ",
              backend, (unsigned long long)kChunks, kRing, sizeof(kShapes) / sizeof(kShapes[0]));
}

// --- the deterministic test completion adapter: records scripted per chunk (errors, missing GPU
// times), no GPU. It checks the interface's contract as CommandQueue uses it: one chunk at a time
// through begin/encode/commit, wait and retire in sequence order with the last wait's token, whose
// records stay as wait() returned them until retire; drain() settles what was committed and never
// retired.
struct ScriptedSubmission final : ChunkSubmission {
  std::vector<std::vector<CommitRecord>> script; // by seq
  std::vector<CommitRecord> records;
  uint64_t begun = 0, committed = 0, waited = 0, retired = 0, drained = 0;
  int contract = 0; // violations

  void
  begin(uint64_t seq) override {
    contract += seq != committed + 1;
    begun = seq;
  }
  void
  waitEvent(WMT::Event, uint64_t) override {}
  void
  signalEvent(WMT::Event, uint64_t) override {}
  void
  encode(uint64_t seq, ChunkEncoder &) override {
    contract += seq != begun;
  }
  void
  commit(uint64_t seq) override {
    contract += seq != begun;
    committed = seq;
  }
  CompletedChunk
  wait(uint64_t seq, uint64_t, bool) override {
    contract += seq != retired + 1 || seq > committed;
    waited = seq;
    records = script[seq];
    return issue(seq, records, gw2::ReportOrder::CommitOrder);
  }
  // gw2 (#91): only with this adapter's token of its last wait (ChunkSubmission::retire checks it)
  void
  retireChunk(const CompletedChunk &chunk) override {
    contract += chunk.seq() != waited || chunk.records().data() != records.data() ||
                chunk.records().size() != script[chunk.seq()].size();
    retired = chunk.seq();
  }
  gw2::DrainedQueue
  drain() override {
    drained = committed - retired;
    retired = committed;
    return gw2::CompletionIssuer::drained();
  }
};

struct NullEncoder final : ChunkEncoder {
  void
  encode(WMT::CommandBuffer &) override {}
  void
  encode(Mtl4Queue &) override {}
};

// device-error lines as CommandQueue::ReportCompletedChunk writes them, through the bridge's description
struct LineSink {
  std::vector<std::string> lines;
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  std::vector<uint64_t> aftergpu;
  std::chrono::steady_clock::time_point now_{std::chrono::seconds(100)};

  std::chrono::steady_clock::time_point
  now() {
    return now_;
  }
  void
  deviceError(const CommitRecord &record) {
    lines.push_back("device: " + WMT::Error(record.error).description().getUTF8String());
  }
  void
  lastLog(const CommitRecord &, const std::string &log) {
    lines.push_back("log: " + log);
  }
  void
  splitError(const CommitRecord &record, size_t index, size_t count, uint64_t errors, uint64_t not_logged) {
    lines.push_back("split " + std::to_string(index + 1) + "/" + std::to_string(count) + " #" + std::to_string(errors) +
                    " (" + std::to_string(not_logged) + " not logged): " +
                    WMT::Error(record.error).description().getUTF8String());
  }
  void
  gpuSpan(uint64_t start, uint64_t end) {
    spans.emplace_back(start, end);
  }
  void
  presentGpuEnd(uint64_t end) {
    aftergpu.push_back(end);
  }
};

static void
scripted() {
  NSMutableArray *errors = [NSMutableArray array]; // keeps the injected errors alive
  auto error = [&](int code) {
    NSError *e = [NSError errorWithDomain:@"gw2.test" code:code userInfo:nil];
    [errors addObject:e];
    return WMT::Error{HANDLE(e)};
  };
  constexpr uint64_t kScripted = 6;
  ScriptedSubmission sub;
  ForeignIssuer foreign;
  sub.script.resize(kScripted + 3);
  for (uint64_t seq = 1; seq <= kScripted + 2; seq++) {
    // three commits: the two split buffers fail on every chunk; the last one on chunk 3; the first
    // commit has no GPU start, the second no end
    for (uint32_t i = 0; i < 3; i++) {
      CommitRecord r;
      r.ordinal = i;
      r.gpu_start_ns = i == 0 ? std::nullopt : std::optional<uint64_t>(seq * 1000 + i * 10);
      r.gpu_end_ns = i == 1 ? std::nullopt : std::optional<uint64_t>(seq * 1000 + i * 10 + 5);
      if (i < 2 || seq == 3)
        r.error = error(int(seq * 10 + i));
      sub.script[seq].push_back(std::move(r));
    }
  }
  gw2::BasicSplitErrorThrottle<std::chrono::steady_clock::time_point> throttle;
  LineSink sink;
  NullEncoder encoder;
  for (uint64_t seq = 1; seq <= kScripted + 2; seq++) {
    sub.begin(seq);
    sub.encode(seq, encoder);
    sub.commit(seq);
    if (seq > kScripted)
      continue; // committed, never waited for: drain()
    auto completed = sub.wait(seq, 0, true);
    gw2::reportCompletedChunk(completed.records(), completed.order(), true, throttle, sink);
    // gw2 (#91): another issuer's token, and the right one twice: only the first of its own is taken
    CHECK(!sub.retire(foreign.token(seq)), "scripted: retire() took another issuer's token");
    CHECK(sub.retired == seq - 1, "scripted: a refused token retired chunk %llu", (unsigned long long)seq);
    CHECK(sub.retire(std::move(completed)), "scripted: retire() refused its own token");
    CHECK(!sub.retire(std::move(completed)), "scripted: retire() took an empty token");
  }
  CHECK(bool(sub.drain()), "scripted: drain() gave no token");
  CHECK(sub.contract == 0, "scripted: %d contract violations", sub.contract);
  CHECK(sub.drained == 2, "scripted: drain() settled %llu chunks, want 2", (unsigned long long)sub.drained);
  // 12 split errors at one instant: the first 8 get a line; chunk 3's last buffer always does
  size_t split_lines = 0, device_lines = 0;
  for (auto &l : sink.lines) {
    split_lines += l.rfind("split ", 0) == 0;
    device_lines += l.rfind("device: ", 0) == 0;
  }
  CHECK(split_lines == 8 && device_lines == 1, "scripted: %zu split lines (want 8), %zu device lines (want 1)",
        split_lines, device_lines);
  CHECK(!sink.lines.empty() && sink.lines[0].find("gw2.test") != std::string::npos &&
            sink.lines[0].rfind("split 1/2 #1 (0 not logged)", 0) == 0,
        "scripted: first line \"%s\"", sink.lines.empty() ? "" : sink.lines[0].c_str());
  CHECK(sink.spans.size() == 3 * kScripted && sink.aftergpu.size() == kScripted, "scripted: %zu spans, %zu aftergpu",
        sink.spans.size(), sink.aftergpu.size());
  CHECK(sink.spans[0] == std::make_pair(uint64_t(0), uint64_t(1005)) &&
            sink.spans[1] == std::make_pair(uint64_t(1010), uint64_t(0)),
        "scripted: a missing GPU time is reported as 0");
  CHECK(sink.aftergpu[0] == 1025, "scripted: aftergpu is the last commit's end");
  std::printf("%s scripted adapter: %llu chunks with failing commits and missing times, 2 drained\n",
              failures ? "...." : "ok  ", (unsigned long long)kScripted);
}

// gw2 (#91, the review of #97): a stale token of the adapter's own: chunk 1's token, kept while the
// adapter already waited for chunk 2. retire() refuses it (not its last wait's), leaves it with the
// caller unchanged, and calls nothing of the adapter's; the last wait's token is then taken back, and
// the stale one stays refused. An isolated adapter: its out-of-order wait would break run()'s contract.
static void
scriptedStaleToken() {
  ScriptedSubmission sub;
  sub.script.resize(3);
  sub.script[1].resize(1);
  sub.script[2].resize(1);
  NullEncoder encoder;
  for (uint64_t seq = 1; seq <= 2; seq++) {
    sub.begin(seq);
    sub.encode(seq, encoder);
    sub.commit(seq);
  }
  auto stale = sub.wait(1, 0, false);
  auto last = sub.wait(2, 0, false);
  int contract = sub.contract; // the second wait broke the in-order contract on purpose
  CHECK(sub.issued(stale) && stale.seq() == 1, "stale token: chunk 1's token isn't the adapter's");
  CHECK(!sub.retire(std::move(stale)), "stale token: retire() took chunk 1's token after chunk 2's wait");
  CHECK(stale && stale.seq() == 1 && stale.issuer() == &sub, "stale token: a refused retire() emptied the token");
  CHECK(sub.retired == 0 && sub.contract == contract, "stale token: a refused retire() reached retireChunk");
  CHECK(sub.retire(std::move(last)) && sub.retired == 2, "stale token: retire() refused the last wait's token");
  CHECK(!sub.retire(std::move(stale)) && stale && sub.retired == 2,
        "stale token: retire() took chunk 1's token after chunk 2 retired");
  std::printf("%s scripted adapter: a stale token of its own is refused and kept\n", failures ? "...." : "ok  ");
}

int
main() {
  @autoreleasepool {
    id<MTLDevice> mtl_device = MTLCreateSystemDefaultDevice();
    if (!mtl_device) {
      std::printf("SKIP: no Metal device\n");
      return 77;
    }
    WMT::Device device{HANDLE(mtl_device)};
    auto completion = device.newSharedEvent();
    scripted();
    scriptedStaleToken();

    {
      auto metal3_queue = device.newCommandQueue(64);
      Chunks chunks;
      Metal3Submission metal3(
          kRing,
          {
              .new_command_buffer = [&] { return WMT::CommandQueue(metal3_queue).commandBuffer(); },
              .before_commit = gw2::residencyBeforeCommit,
              .split_buffers = [&](uint64_t seq) { return chunks.ledger.splits(seq); },
              .wait_at_drain = [] { return true; },
          }
      );
      run("Metal 3", nullptr, metal3, metal3_queue, chunks, mtl_device, completion);
    }

    if (device.metal4Support() != WMTMetal4Supported) {
      std::printf("SKIP: no Metal 4 here (the Metal 3 half ran)\n");
      return failures ? 1 : 77;
    }
    {
      auto queue = std::make_unique<Mtl4Queue>(device, kRing);
      CHECK(queue->valid(), "Mtl4Queue isn't valid");
      if (!queue->valid())
        return 1;
      Chunks chunks;
      Mtl4Queue *mtl4 = queue.get();
      Metal4Submission metal4(std::move(queue), completion);
      run("Metal 4", mtl4, metal4, WMT::CommandQueue{}, chunks, mtl_device, completion);
    }
  }
  if (failures) {
    std::printf("%d failures\n", failures.load());
    return 1;
  }
  std::printf("chunk_submission_test: all passed\n");
  return 0;
}
