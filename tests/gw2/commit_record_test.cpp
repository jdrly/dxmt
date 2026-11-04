// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O1 -Wall -Wextra -Isrc/dxmt tests/gw2/commit_record_test.cpp -o $OUT
//
// gw2 (#77, Q1, architecture council §2): native test of the commit records
// (src/dxmt/dxmt_gw2_commit_record.hpp) with scripted command buffers and Metal 4 commits. No Metal,
// no Wine, no threads.
//
// - Parity: the finish thread's two bodies as gw2-on-mac 13007ad wrote them (the "legacy oracle"
//   below: WaitForFinishThread's Metal 3 branch, FinishChunkMetal4's report, ReportSplitError and
//   SplitErrorLine) and the records (recordMetal3Chunk, as Metal3Submission::wait builds them, and
//   recordMetal4Commit, then reportCompletedChunk) run the same scripted chunks: 0-3 split buffers or 0-4 commits, pending or
//   completed buffers, GPU errors anywhere, logs, missing GPU times, DXMT_STATS on and off, the split
//   error throttle on a scripted clock. The device-error lines, the GPU line's spans and PRESENT's
//   aftergpu must be identical, in order, over the whole run. Every buffer is waited for exactly when
//   the oracle waits for it and in the same order (the last buffer first, then the split buffers in
//   commit order; gw2 #78), nothing is read from a buffer before it completed, and the records make
//   no command-buffer call the oracle doesn't, except an error or logs read of a failed split buffer
//   whose line the throttle drops (the error path only; nothing more is logged).
// - Records: ordinals, known and unknown GPU times (0 is unknown), errors and logs.
// - The throttle: the first 8 lines, then one a second with the count not logged.
// - The report: Metal 3's line order (the last buffer first) and Metal 4's (commit order), nothing for
//   a chunk with no commits.
//
// Run: tests/gw2/run.zsh pure, or the command above with $OUT set.

#include "dxmt_gw2_commit_record.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace dxmt::gw2;
using TimePoint = std::chrono::steady_clock::time_point;

static int failures = 0;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                  \
      std::printf(__VA_ARGS__);                                                                    \
      std::printf("\n");                                                                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

template <class... Args>
static std::string
fmt(const Args &...args) {
  std::ostringstream s;
  (s << ... << args);
  return s.str();
}

// --- the scripted world

// what the chunk's report wrote: device-error lines, and the GPU line's and PRESENT's values
struct Output {
  std::vector<std::string> lines;
  std::vector<std::string> values; // "span <frame> <start> <end>", "aftergpu <frame> <end>"
};

// every call made on a scripted command buffer: "<call> <buffer id>"
struct Calls {
  std::vector<std::string> list;
  std::vector<std::string> early; // a read before the buffer completed
};

static TimePoint g_now; // the scripted clock (the finish thread's clock::now())

struct FakeError {
  int id = 0;
  explicit
  operator bool() const {
    return id != 0;
  }
};

// a Metal 3 command buffer, as committed: pending (committed or scheduled) until waited for, then
// completed or failed
struct BufferScript {
  int id;
  bool pending;
  int pending_status; // 2 committed, 3 scheduled
  bool failed;
  std::vector<std::string> logs;
  uint64_t gpu_start, gpu_end; // ns, 0: none
};

struct FakeCmdbuf {
  static constexpr int kScheduled = 3; // WMTCommandBufferStatus
  static constexpr int kCompleted = 4;
  static constexpr int kError = 5;

  const BufferScript *s;
  Calls *calls;
  bool done;

  FakeCmdbuf(const BufferScript &script, Calls &c) : s(&script), calls(&c), done(!script.pending) {}

  void
  note(const char *call) {
    calls->list.push_back(fmt(call, " ", s->id));
  }
  void
  read(const char *call) {
    note(call);
    if (!done)
      calls->early.push_back(fmt(call, " ", s->id));
  }

  int
  status() {
    note("status");
    return done ? (s->failed ? kError : kCompleted) : s->pending_status;
  }
  void
  waitUntilCompleted() {
    note("wait");
    done = true;
  }
  FakeError
  error() {
    read("error");
    return {s->failed ? 100 + s->id : 0};
  }
  // the records' form (Metal3Buffer::logs): logs() and its elements' descriptions
  void
  logs(std::vector<std::string> &out) {
    read("logs");
    out.insert(out.end(), s->logs.begin(), s->logs.end());
  }
  // the oracle's form (WMT's logs().elements(), each description): the same reads
  std::vector<std::string>
  logs() {
    read("logs");
    return s->logs;
  }
  uint64_t
  gpuStartTime() {
    read("start");
    return s->gpu_start;
  }
  uint64_t
  gpuEndTime() {
    read("end");
    return s->gpu_end;
  }
};

static std::string
describe(FakeError e, Calls &calls) {
  calls.list.push_back(fmt("describe ", e.id));
  return fmt("error ", e.id);
}

// a Metal 4 commit with its feedback written (WMTMtl4CommitFeedback), as Mtl4Commit reads it
struct FakeMtl4Commit {
  struct {
    double gpu_start;
    double gpu_end;
    int error;
  } feedback;

  uint64_t
  gpuStartNs() const {
    return feedback.gpu_start > 0 ? uint64_t(feedback.gpu_start * 1e9) : 0;
  }
  uint64_t
  gpuEndNs() const {
    return feedback.gpu_end > 0 ? uint64_t(feedback.gpu_end * 1e9) : 0;
  }
  FakeError
  error() const {
    return {feedback.error};
  }
};

// --- the legacy oracle: gw2-on-mac 13007ad's finish thread, on the scripted buffers

struct LegacyQueue {
  uint64_t split_errors_ = 0;
  uint64_t split_errors_logged_ = 0;
  TimePoint split_error_last_line_{};

  bool
  SplitErrorLine(uint64_t &not_logged) {
    split_errors_++;
    auto now = g_now;
    if (split_errors_ > 8 && now - split_error_last_line_ < std::chrono::seconds(1))
      return false;
    not_logged = split_errors_ - split_errors_logged_ - 1;
    split_errors_logged_ = split_errors_;
    split_error_last_line_ = now;
    return true;
  }

  void
  ReportSplitError(uint64_t frame, uint64_t seq, size_t index, size_t count, FakeCmdbuf cmdbuf, Output &out) {
    uint64_t not_logged;
    if (!SplitErrorLine(not_logged))
      return;
    out.lines.push_back(fmt("Device error at frame ", frame, " (chunk ", seq, ", split-off buffer ", index + 1, " of ",
                            count, ", split error ", split_errors_, ", ", not_logged,
                            " not logged since the last line): ", describe(cmdbuf.error(), *cmdbuf.calls)));
    for (auto &log : cmdbuf.logs())
      out.lines.push_back(fmt("Frame ", frame, " (chunk ", seq, ", split-off buffer ", index + 1, "): ", log));
  }

  void
  ReportSplitError(uint64_t frame, uint64_t seq, size_t index, size_t count, FakeError error, Calls &calls, Output &out) {
    uint64_t not_logged;
    if (!SplitErrorLine(not_logged))
      return;
    out.lines.push_back(fmt("Device error at frame ", frame, " (chunk ", seq, ", split-off buffer ", index + 1, " of ",
                            count, ", split error ", split_errors_, ", ", not_logged,
                            " not logged since the last line): ", describe(error, calls)));
  }

  // WaitForFinishThread's Metal 3 branch
  void
  finishMetal3(uint64_t frame, uint64_t seq, FakeCmdbuf attached_cmdbuf, std::vector<FakeCmdbuf> splits, bool stats,
               Output &out) {
    if (attached_cmdbuf.status() <= FakeCmdbuf::kScheduled) {
      attached_cmdbuf.waitUntilCompleted();
    }
    if (attached_cmdbuf.status() == FakeCmdbuf::kError) {
      out.lines.push_back(fmt("Device error at frame ", frame, ": ", describe(attached_cmdbuf.error(), *attached_cmdbuf.calls)));
    }
    for (auto &log : attached_cmdbuf.logs()) {
      out.lines.push_back(fmt("Frame ", frame, ": ", log));
    }
    for (size_t i = 0; i < splits.size(); i++) {
      auto &split = splits[i];
      auto status = split.status();
      if (status <= FakeCmdbuf::kScheduled) {
        split.waitUntilCompleted();
        status = split.status();
      }
      if (status == FakeCmdbuf::kError)
        ReportSplitError(frame, seq, i, splits.size(), split, out);
      if (stats)
        out.values.push_back(fmt("span ", frame, " ", split.gpuStartTime(), " ", split.gpuEndTime()));
    }
    if (stats) {
      uint64_t gpu_end = attached_cmdbuf.gpuEndTime();
      out.values.push_back(fmt("span ", frame, " ", attached_cmdbuf.gpuStartTime(), " ", gpu_end));
      out.values.push_back(fmt("aftergpu ", frame, " ", gpu_end));
    }
  }

  // FinishChunkMetal4's report (after its waits)
  void
  finishMetal4(uint64_t frame, uint64_t seq, const std::vector<FakeMtl4Commit> &commits, bool stats, Calls &calls,
               Output &out) {
    size_t splits = commits.empty() ? 0 : commits.size() - 1;
    for (size_t i = 0; i < commits.size(); i++) {
      auto *commit = &commits[i];
      if (commit->feedback.error != 0) {
        if (i < splits)
          ReportSplitError(frame, seq, i, splits, commit->error(), calls, out);
        else
          out.lines.push_back(fmt("Device error at frame ", frame, ": ", describe(commit->error(), calls)));
      }
      if (stats)
        out.values.push_back(fmt("span ", frame, " ", commit->gpuStartNs(), " ", commit->gpuEndNs()));
    }
    if (stats && !commits.empty())
      out.values.push_back(fmt("aftergpu ", frame, " ", commits.back().gpuEndNs()));
  }
};

// --- the records: what CommandQueue::FinishChunkMetal3 / FinishChunkMetal4 and ReportCompletedChunk do

using Record = BasicCommitRecord<FakeError>;

struct Sink {
  uint64_t frame;
  uint64_t seq;
  Calls &calls;
  Output &out;

  TimePoint
  now() {
    return g_now;
  }
  void
  deviceError(const Record &record) {
    out.lines.push_back(fmt("Device error at frame ", frame, ": ", describe(record.error, calls)));
  }
  void
  lastLog(const Record &, const std::string &log) {
    out.lines.push_back(fmt("Frame ", frame, ": ", log));
  }
  void
  splitError(const Record &record, size_t index, size_t count, uint64_t errors, uint64_t not_logged) {
    out.lines.push_back(fmt("Device error at frame ", frame, " (chunk ", seq, ", split-off buffer ", index + 1, " of ",
                            count, ", split error ", errors, ", ", not_logged, " not logged since the last line): ",
                            describe(record.error, calls)));
    for (auto &log : record.logs)
      out.lines.push_back(fmt("Frame ", frame, " (chunk ", seq, ", split-off buffer ", index + 1, "): ", log));
  }
  void
  gpuSpan(uint64_t start, uint64_t end) {
    out.values.push_back(fmt("span ", frame, " ", start, " ", end));
  }
  void
  presentGpuEnd(uint64_t end) {
    out.values.push_back(fmt("aftergpu ", frame, " ", end));
  }
};

struct RecordQueue {
  BasicSplitErrorThrottle<TimePoint> split_errors_;
  std::vector<Record> completed_;

  void
  finishMetal3(uint64_t frame, uint64_t seq, FakeCmdbuf last, std::vector<FakeCmdbuf> splits, bool stats, Calls &calls,
               Output &out) {
    // as Metal3Submission::wait (gw2 #78)
    recordMetal3Chunk(splits.size(), [&](size_t i) { return i < splits.size() ? splits[i] : last; }, stats, completed_);
    Sink sink{frame, seq, calls, out};
    reportCompletedChunk(std::span<const Record>(completed_), ReportOrder::LastFirst, stats, split_errors_, sink);
  }

  void
  finishMetal4(uint64_t frame, uint64_t seq, const std::vector<FakeMtl4Commit> &commits, bool stats, Calls &calls,
               Output &out) {
    completed_.resize(commits.size());
    for (size_t i = 0; i < commits.size(); i++)
      recordMetal4Commit(commits[i], uint32_t(i), completed_[i]);
    Sink sink{frame, seq, calls, out};
    reportCompletedChunk(std::span<const Record>(completed_), ReportOrder::CommitOrder, stats, split_errors_, sink);
  }
};

// --- scripted chunks

struct Chunk {
  uint64_t frame, seq;
  bool stats;
  int64_t advance_ms; // the clock moves this much before the chunk finishes
  std::vector<BufferScript> splits;
  BufferScript last;
  std::vector<FakeMtl4Commit> commits;
};

static std::multimap<std::string, int>
counts(const std::vector<std::string> &calls) {
  std::multimap<std::string, int> m;
  for (auto &c : calls)
    if (c.rfind("status ", 0) != 0)
      m.emplace(c, 0);
  return m;
}

static std::vector<std::string>
waits(const std::vector<std::string> &calls) {
  std::vector<std::string> w;
  for (auto &c : calls)
    if (c.rfind("wait ", 0) == 0)
      w.push_back(c);
  return w;
}

static size_t
countOf(const std::vector<std::string> &calls, const char *prefix) {
  return std::count_if(calls.begin(), calls.end(), [&](auto &c) { return c.rfind(prefix, 0) == 0; });
}

// Runs `chunks` through the oracle and the records; the outputs must be identical, and the calls on
// every chunk's buffers as described in the header. Returns the oracle's output.
static Output
parityMetal3(const char *name, const std::vector<Chunk> &chunks) {
  LegacyQueue legacy;
  RecordQueue records;
  Output out_legacy, out_records;
  TimePoint start{std::chrono::seconds(10)};
  g_now = start;
  int bad_chunks = 0;
  for (auto &c : chunks) {
    g_now += std::chrono::milliseconds(c.advance_ms);
    Calls cl, cr;
    std::vector<FakeCmdbuf> sl, sr;
    for (auto &s : c.splits) {
      sl.emplace_back(s, cl);
      sr.emplace_back(s, cr);
    }
    legacy.finishMetal3(c.frame, c.seq, FakeCmdbuf(c.last, cl), sl, c.stats, out_legacy);
    records.finishMetal3(c.frame, c.seq, FakeCmdbuf(c.last, cr), sr, c.stats, cr, out_records);

    bool ok = true;
    ok &= cr.early.empty();
    // the waits in the oracle's order: the last buffer first, then the split buffers in commit order
    ok &= waits(cr.list) == waits(cl.list);
    for (auto &s : c.splits)
      ok &= countOf(cr.list, fmt("wait ", s.id).c_str()) == (s.pending ? 1u : 0u);
    ok &= countOf(cr.list, fmt("wait ", c.last.id).c_str()) == (c.last.pending ? 1u : 0u);
    ok &= countOf(cr.list, "status ") <= countOf(cl.list, "status ");
    // every call but status: the oracle's, plus at most an error and a logs read of a failed split
    auto ml = counts(cl.list), mr = counts(cr.list);
    for (auto &[call, _] : ml) {
      if (mr.count(call) < ml.count(call))
        ok = false;
    }
    for (auto &[call, _] : mr) {
      if (mr.count(call) <= ml.count(call))
        continue;
      bool extra_allowed = false;
      for (auto &s : c.splits)
        if (s.failed && (call == fmt("error ", s.id) || call == fmt("logs ", s.id)))
          extra_allowed = mr.count(call) == 1;
      ok &= extra_allowed;
    }
    if (!ok && bad_chunks++ < 3) {
      std::printf("  %s: chunk %llu: calls differ\n    oracle :", name, (unsigned long long)c.seq);
      for (auto &x : cl.list)
        std::printf(" [%s]", x.c_str());
      std::printf("\n    records:");
      for (auto &x : cr.list)
        std::printf(" [%s]", x.c_str());
      std::printf("\n");
    }
  }
  CHECK(bad_chunks == 0, "%s: %d chunks with calls not as the oracle's", name, bad_chunks);
  CHECK(out_records.lines == out_legacy.lines, "%s: the device-error lines differ (%zu vs %zu)", name,
        out_records.lines.size(), out_legacy.lines.size());
  CHECK(out_records.values == out_legacy.values, "%s: the GPU spans or aftergpu differ (%zu vs %zu)", name,
        out_records.values.size(), out_legacy.values.size());
  if (out_records.lines != out_legacy.lines)
    for (size_t i = 0; i < std::max(out_records.lines.size(), out_legacy.lines.size()) && i < 6; i++)
      std::printf("    [%zu] oracle %s\n         records %s\n", i, i < out_legacy.lines.size() ? out_legacy.lines[i].c_str() : "-",
                  i < out_records.lines.size() ? out_records.lines[i].c_str() : "-");
  return out_legacy;
}

static Output
parityMetal4(const char *name, const std::vector<Chunk> &chunks) {
  LegacyQueue legacy;
  RecordQueue records;
  Output out_legacy, out_records;
  g_now = TimePoint{std::chrono::seconds(10)};
  int bad = 0;
  for (auto &c : chunks) {
    g_now += std::chrono::milliseconds(c.advance_ms);
    Calls cl, cr;
    legacy.finishMetal4(c.frame, c.seq, c.commits, c.stats, cl, out_legacy);
    records.finishMetal4(c.frame, c.seq, c.commits, c.stats, cr, out_records);
    if (cl.list != cr.list)
      bad++;
  }
  CHECK(bad == 0, "%s: %d chunks whose error descriptions differ", name, bad);
  CHECK(out_records.lines == out_legacy.lines, "%s: the device-error lines differ (%zu vs %zu)", name,
        out_records.lines.size(), out_legacy.lines.size());
  CHECK(out_records.values == out_legacy.values, "%s: the GPU spans or aftergpu differ (%zu vs %zu)", name,
        out_records.values.size(), out_legacy.values.size());
  return out_legacy;
}

// --- generated runs

struct Mix {
  double pending, failed, logs, no_time, stats;
  int max_advance_ms;
};

static std::vector<Chunk>
generate(std::mt19937 &rng, int n, Mix mix, bool metal4) {
  std::uniform_real_distribution<double> u(0, 1);
  std::vector<Chunk> chunks;
  uint64_t frame = 1;
  int id = 1;
  auto buffer = [&] {
    BufferScript b{};
    b.id = id++;
    b.pending = u(rng) < mix.pending;
    b.pending_status = u(rng) < 0.5 ? 2 : 3;
    b.failed = u(rng) < mix.failed;
    if (u(rng) < mix.logs)
      for (int k = 0, m = 1 + int(u(rng) * 2); k < m; k++)
        b.logs.push_back(fmt("log ", b.id, ".", k));
    b.gpu_start = u(rng) < mix.no_time ? 0 : 1'000'000'000ull + b.id * 1000;
    b.gpu_end = u(rng) < mix.no_time ? 0 : 1'000'000'000ull + b.id * 1000 + 700;
    return b;
  };
  for (int i = 0; i < n; i++) {
    Chunk c{};
    c.seq = i + 1;
    if (u(rng) < 0.4)
      frame++;
    c.frame = frame;
    c.stats = u(rng) < mix.stats;
    c.advance_ms = int64_t(u(rng) * mix.max_advance_ms);
    if (metal4) {
      int n_commits = int(u(rng) * 5); // 0-4
      for (int k = 0; k < n_commits; k++) {
        FakeMtl4Commit m{};
        m.feedback.gpu_start = u(rng) < mix.no_time ? 0 : 1.0 + id * 1e-6;
        m.feedback.gpu_end = u(rng) < mix.no_time ? (u(rng) < 0.5 ? 0 : -1) : 1.0 + id * 1e-6 + 5e-7;
        m.feedback.error = u(rng) < mix.failed ? 100 + id : 0;
        id++;
        c.commits.push_back(m);
      }
    } else {
      int n_splits = int(u(rng) * 4); // 0-3
      for (int k = 0; k < n_splits; k++)
        c.splits.push_back(buffer());
      c.last = buffer();
    }
    chunks.push_back(std::move(c));
  }
  return chunks;
}

// --- direct checks

static void
testRecords() {
  Calls calls;
  BufferScript pending{.id = 1, .pending = true, .pending_status = 2, .failed = false, .logs = {"a", "b"},
                       .gpu_start = 5, .gpu_end = 9};
  BufferScript failed{.id = 2, .pending = false, .pending_status = 3, .failed = true, .logs = {"c"},
                      .gpu_start = 0, .gpu_end = 0};
  Record r;
  r.logs = {"stale"};
  FakeCmdbuf b1(pending, calls);
  recordMetal3Commit(b1, 3, true, true, r);
  CHECK(r.ordinal == 3 && !r.error && r.logs == std::vector<std::string>({"a", "b"}), "the last buffer's record");
  CHECK(r.gpu_start_ns == 5u && r.gpu_end_ns == 9u, "known GPU times");
  CHECK(countOf(calls.list, "wait 1") == 1 && calls.early.empty(), "a pending buffer is waited for before any read");

  FakeCmdbuf b1again(pending, calls);
  recordMetal3Commit(b1again, 0, false, false, r);
  CHECK(r.logs.empty() && !r.gpu_start_ns && !r.gpu_end_ns, "a split buffer without an error: no logs; no timing asked");

  FakeCmdbuf b2(failed, calls);
  recordMetal3Commit(b2, 1, false, true, r);
  CHECK(r.error.id == 102 && r.logs == std::vector<std::string>({"c"}), "a failed split buffer: its error and logs");
  CHECK(!r.gpu_start_ns && !r.gpu_end_ns, "Metal's 0 is an unknown time, not 0");

  FakeMtl4Commit m{};
  m.feedback = {.gpu_start = 2.5, .gpu_end = -1, .error = 7};
  recordMetal4Commit(m, 4, r);
  CHECK(r.ordinal == 4 && r.gpu_start_ns == 2'500'000'000u && !r.gpu_end_ns && r.error.id == 7 && r.logs.empty(),
        "a Metal 4 record from its feedback");
  m.feedback = {.gpu_start = 0, .gpu_end = 0, .error = 0};
  recordMetal4Commit(m, 5, r);
  CHECK(!r.gpu_start_ns && !r.gpu_end_ns && !r.error, "Metal 4 feedback without times or an error");
}

static void
testThrottle() {
  BasicSplitErrorThrottle<TimePoint> t;
  TimePoint now{std::chrono::seconds(100)};
  uint64_t not_logged = 99;
  int lines = 0;
  for (int i = 0; i < 20; i++) {
    if (t.line(now, not_logged)) {
      lines++;
      CHECK(not_logged == 0, "the first 8 lines miss nothing (line %d: %llu)", lines, (unsigned long long)not_logged);
    }
    now += std::chrono::milliseconds(10);
  }
  CHECK(lines == 8, "20 errors in 0.2 s: 8 lines, got %d", lines);
  now += std::chrono::seconds(1);
  CHECK(t.line(now, not_logged) && not_logged == 12, "a second later: a line with the 12 not logged (%llu)",
        (unsigned long long)not_logged);
  CHECK(!t.line(now + std::chrono::milliseconds(999), not_logged), "then none within the second");
  CHECK(t.line(now + std::chrono::seconds(1), not_logged) && not_logged == 1 && t.errors() == 23, "then one again");
}

static void
testOrder() {
  // the last buffer failed with a log, the second of three split buffers too
  Chunk c{.frame = 7, .seq = 42, .stats = true, .advance_ms = 0};
  c.splits = {{.id = 1, .pending = false, .pending_status = 2, .failed = false, .logs = {}, .gpu_start = 10, .gpu_end = 20},
              {.id = 2, .pending = true, .pending_status = 3, .failed = true, .logs = {"s"}, .gpu_start = 0, .gpu_end = 0},
              {.id = 3, .pending = true, .pending_status = 2, .failed = false, .logs = {}, .gpu_start = 30, .gpu_end = 40}};
  c.last = {.id = 4, .pending = true, .pending_status = 2, .failed = true, .logs = {"l"}, .gpu_start = 50, .gpu_end = 60};
  auto out = parityMetal3("metal3 order", {c});
  std::vector<std::string> lines = {
      "Device error at frame 7: error 104",
      "Frame 7: l",
      "Device error at frame 7 (chunk 42, split-off buffer 2 of 3, split error 1, 0 not logged since the last line): error 102",
      "Frame 7 (chunk 42, split-off buffer 2): s",
  };
  std::vector<std::string> values = {"span 7 10 20", "span 7 0 0", "span 7 30 40", "span 7 50 60", "aftergpu 7 60"};
  CHECK(out.lines == lines, "Metal 3: the last buffer's error and logs, then the split buffers'");
  CHECK(out.values == values, "Metal 3: the split buffers' spans, the last one's, its end (0 for none)");

  Chunk m{.frame = 3, .seq = 9, .stats = true, .advance_ms = 0};
  m.commits = {{{1.0, 1.5, 0}}, {{2.0, 0, 11}}, {{3.0, 3.5, 12}}};
  out = parityMetal4("metal4 order", {m});
  lines = {
      "Device error at frame 3 (chunk 9, split-off buffer 2 of 2, split error 1, 0 not logged since the last line): error 11",
      "Device error at frame 3: error 12",
  };
  values = {"span 3 1000000000 1500000000", "span 3 2000000000 0", "span 3 3000000000 3500000000",
            "aftergpu 3 3500000000"};
  CHECK(out.lines == lines, "Metal 4: commit order");
  CHECK(out.values == values, "Metal 4: spans and aftergpu");

  Chunk empty{.frame = 1, .seq = 1, .stats = true, .advance_ms = 0};
  out = parityMetal4("metal4 empty", {empty});
  CHECK(out.lines.empty() && out.values.empty(), "a Metal 4 chunk with no commits reports nothing");

  Chunk quiet = c;
  quiet.stats = false;
  out = parityMetal3("metal3 no stats", {quiet});
  CHECK(out.values.empty() && out.lines.size() == 4, "without DXMT_STATS: the lines, no values");
}

int
main() {
  testRecords();
  testThrottle();
  testOrder();

  struct Run {
    const char *name;
    Mix mix;
    int chunks;
  } runs[] = {
      {"quiet", {.pending = 0.5, .failed = 0.0, .logs = 0.0, .no_time = 0.05, .stats = 0.5, .max_advance_ms = 20}, 20000},
      {"stats on", {.pending = 0.7, .failed = 0.01, .logs = 0.02, .no_time = 0.1, .stats = 1.0, .max_advance_ms = 20}, 20000},
      {"faulting", {.pending = 0.5, .failed = 0.3, .logs = 0.3, .no_time = 0.3, .stats = 0.5, .max_advance_ms = 300}, 20000},
      {"fault storm", {.pending = 0.3, .failed = 0.9, .logs = 0.5, .no_time = 0.5, .stats = 0.5, .max_advance_ms = 1500}, 5000},
  };
  for (unsigned seed = 1; seed <= 3; seed++) {
    for (auto &run : runs) {
      std::mt19937 rng(seed * 7919 + run.chunks);
      auto name = fmt(run.name, " seed ", seed);
      auto m3 = parityMetal3(fmt("metal3 ", name).c_str(), generate(rng, run.chunks, run.mix, false));
      auto m4 = parityMetal4(fmt("metal4 ", name).c_str(), generate(rng, run.chunks, run.mix, true));
      if (seed == 1)
        std::printf("ok   parity %-12s: Metal 3 %zu lines %zu values, Metal 4 %zu lines %zu values (%d chunks each)\n",
                    run.name, m3.lines.size(), m3.values.size(), m4.lines.size(), m4.values.size(), run.chunks);
    }
  }

  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("commit_record_test: all passed\n");
  return 0;
}
