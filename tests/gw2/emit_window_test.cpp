// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Itests/gw2/shim -Isrc/dxmt -Isrc/util -Isrc/winemetal tests/gw2/emit_window_test.cpp -o $OUT
//
// gw2 (#148): the emit window (src/dxmt/dxmt_gw2_emit_window.hpp) over the fork's own ring allocator
// (RingBumpState with HostBufferBlockAllocator and null_mutex, as cpu_command_allocator) and command
// list. No Metal, no Wine. The fake queue does what CommandQueue does around the window:
// CurrentChunk is chunks[seq % n], AllocateCommandData asks the ring with the current and coherent
// sequences, commit() empties the window, moves to the next chunk and frees the ring's blocks
// (CommitCurrentChunk), and complete() runs and resets a chunk and makes it coherent (the finish
// thread). Its ring has 64 KB blocks, not 4 MB, so blocks fill and get reused within the test.
//
// Every allocation the ring hands out is checked against the commands of the chunks that haven't
// completed: none may overlap one. Every command checks its chunk and its payload when its chunk runs.
//
// - commitMidRun: a commit in the middle of a run of emits. The commands before it are in the first
//   chunk's list, the ones after in the second's, each in a slab taken while its own chunk was current.
// - reuseAfterCompletion: three chunks emit a few commands each, the first two complete, and another
//   user takes most of a block. A window that outlived its commit would leave the third chunk's
//   commands in a block stamped with the first, which the ring reuses at once.
// - oversized: a command larger than a slab takes its own memory and the window stays as it was.
// - randomRun: 300,000 random steps of emits of three sizes, other users' allocations (the immediate
//   context's command data, the present), commits at any point and completions lagging behind.
// - bench: ns per emit for the path before #148 (CurrentChunk, then an out-of-line ring allocation
//   per command) and through the window, 60,000 emits a chunk. Printed, not checked.

#include "dxmt_command_list.hpp"
#include "dxmt_gw2_emit_window.hpp"
#include "dxmt_ring_bump_allocator.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dxmt;

static int failures = 0;
static int checks = 0;
static bool quiet = false; // a mutant run: its failures are expected and counted, not printed
#define CHECK(...)                                                                                 \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(__VA_ARGS__)) {                                                                          \
      if (!quiet)                                                                                  \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

using Window = gw2::EmitWindow<struct Chunk>;

// what a chunk's commands see when it runs
struct Ctx {
  uint64_t seq = 0;
  uint64_t next = 0; // the index the next command should carry
  uint64_t bad = 0;
};

struct Live {
  uintptr_t begin, end;
};

struct Chunk {
  CommandList<Ctx> list;
  uint64_t seq = 0;
  uint64_t emitted = 0;
  std::vector<Live> live; // its commands' memory, until it completes
};

static uint64_t
pattern(uint64_t seq, uint64_t idx, uint64_t i) {
  return (seq * 0x9E3779B97F4A7C15ull) ^ (idx << 20) ^ i;
}

struct Take {
  uintptr_t begin, end;
  uint64_t seq;
};

struct Queue {
  static constexpr size_t kBlock = 4 * Window::kSlab;
  static constexpr uint32_t kChunks = 4;

  RingBumpState<HostBufferBlockAllocator, kBlock, null_mutex> ring{{}};
  std::array<Chunk, kChunks> chunks;
  uint64_t ready = 1;    // the current chunk's sequence
  uint64_t coherent = 0; // the last completed one
  Window window;
  std::vector<Take> takes; // what the ring handed out, with the sequence it was taken under
  bool reset_on_commit = true;
  int overlaps = 0;

  Queue() {
    chunks[ready % kChunks].seq = ready;
  }
  ~Queue() {
    while (coherent + 1 < ready)
      complete();
    current().list.reset(); // the open chunk's commands are destroyed before the ring's blocks
  }

  Chunk &
  current() {
    return chunks[ready % kChunks];
  }

  // the window's two calls into the queue, as CommandQueue's
  Chunk *
  CurrentChunk() {
    return &current();
  }
  void *
  AllocateCommandData(size_t size, size_t alignment) {
    auto [block, offset] = ring.allocate(ready, coherent, size, alignment);
    auto p = reinterpret_cast<uintptr_t>(block.ptr) + offset;
    CHECK(p % alignment == 0);
    for (auto &c : chunks) {
      if (c.seq <= coherent)
        continue;
      for (auto &l : c.live) {
        bool overlaps = p < l.end && l.begin < p + size;
        CHECK(!overlaps);
        if (overlaps) {
          this->overlaps++;
          if (!quiet)
            std::printf("  the ring handed out [%zu bytes] under seq %llu over a command of seq %llu (coherent %llu)\n", size,
                      (unsigned long long)ready, (unsigned long long)c.seq, (unsigned long long)coherent);
          return reinterpret_cast<void *>(p);
        }
      }
    }
    takes.push_back({p, p + size, ready});
    return reinterpret_cast<void *>(p);
  }

  template <size_t N>
  void
  emit() {
    Chunk &cur = current();
    uint64_t seq = cur.seq, idx = cur.emitted++;
    std::array<uint64_t, N> payload;
    for (size_t i = 0; i < N; i++)
      payload[i] = pattern(seq, idx, i);
    auto fn = [seq, idx, payload](Ctx &ctx) {
      bool ok = ctx.seq == seq && ctx.next == idx;
      for (size_t i = 0; i < N; i++)
        ok = ok && payload[i] == pattern(seq, idx, i);
      ctx.bad += !ok;
      ctx.next++;
    };
    size_t size = sizeof(impl::LambdaCommand<Ctx, decltype(fn)>);
    auto [chunk, ptr] = window.place(*this, size, 16); // as CommandQueue::Emit
    CHECK(chunk == &cur);
    CHECK(reinterpret_cast<uintptr_t>(ptr) % 16 == 0);
    chunk->list.emit(std::move(fn), ptr);
    auto p = reinterpret_cast<uintptr_t>(ptr);
    cur.live.push_back({p, p + size});
    // a window never spans a commit: the command is in memory taken while its chunk was current
    auto take = std::find_if(takes.rbegin(), takes.rend(), [&](const Take &t) { return t.begin <= p && p + size <= t.end; });
    CHECK(take != takes.rend() && take->seq == seq);
  }

  // another user of the chunk's heap (the immediate context's command data, the present's emitcc)
  void
  other(size_t size) {
    auto p = AllocateCommandData(size, 16);
    std::memset(p, 0xA5, size);
  }

  // CommandQueue::CommitCurrentChunk, with the ring discipline: at most kChunks - 1 in flight
  void
  commit() {
    if (reset_on_commit)
      window.reset();
    ready++;
    if (ready - coherent > kChunks)
      complete();
    Chunk &next = current();
    CHECK(next.live.empty());
    next.seq = ready;
    next.emitted = 0;
    ring.free_blocks(coherent);
  }

  // the finish thread: the oldest committed chunk runs, is reset and becomes coherent
  void
  complete() {
    CHECK(coherent + 1 < ready);
    uint64_t seq = coherent + 1;
    Chunk &c = chunks[seq % kChunks];
    CHECK(c.seq == seq);
    Ctx ctx{.seq = seq};
    c.list.execute(ctx);
    CHECK(ctx.bad == 0);
    CHECK(ctx.next == c.emitted);
    c.list.reset();
    c.live.clear();
    coherent = seq;
  }
};

static void
commitMidRun() {
  Queue q;
  for (int i = 0; i < 100; i++)
    q.emit<3>();
  Chunk *first = q.CurrentChunk();
  q.commit();
  Chunk *second = q.CurrentChunk();
  CHECK(first != second);
  for (int i = 0; i < 100; i++)
    q.emit<3>();
  CHECK(first->emitted == 100);
  CHECK(second->emitted == 100);
  // the second chunk's first command started a slab of its own
  CHECK(q.takes.size() == 2);
  CHECK(q.takes[0].seq == 1 && q.takes[1].seq == 2);
  CHECK(q.takes[1].begin == second->live.front().begin);
  q.complete();
  q.commit();
  q.complete();
}

static void
reuseAfterCompletion(bool reset_on_commit) {
  Queue q;
  q.reset_on_commit = reset_on_commit;
  int before = failures;
  quiet = !reset_on_commit;
  for (int chunk = 0; chunk < 3; chunk++) {
    for (int i = 0; i < 10; i++)
      q.emit<3>();
    if (chunk < 2)
      q.commit();
  }
  q.complete();
  q.complete();
  q.other(Queue::kBlock - 64);
  if (!reset_on_commit) {
    // the mutant: the ring hands the third chunk's memory out again, and the check above sees it
    int overlaps = q.overlaps;
    failures = before;
    quiet = false;
    CHECK(overlaps > 0);
    q.current().list.reset(); // its commands may be overwritten: don't run them
    q.current().live.clear();
    q.chunks[(q.ready - 1) % Queue::kChunks].list.reset();
  }
}

static void
oversized() {
  Queue q;
  q.emit<3>();
  size_t takes = q.takes.size();
  q.emit<2100>(); // 16.8 KB, more than a slab
  CHECK(q.takes.size() == takes + 1);
  CHECK(q.takes.back().end - q.takes.back().begin > Window::kSlab);
  q.emit<3>(); // back in the first slab
  CHECK(q.takes.size() == takes + 1);
  CHECK(q.current().live[2].begin < q.takes[0].end && q.current().live[2].begin >= q.takes[0].begin);
  q.commit();
  q.emit<2100>(); // the first command of a chunk, oversized
  q.emit<3>();
  CHECK(q.takes.back().seq == 2);
}

static void
randomRun() {
  Queue q;
  uint64_t x = 0x2545F4914F6CDD1Dull;
  auto next = [&] {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  };
  for (int step = 0; step < 300000; step++) {
    uint64_t r = next() % 1000;
    if (r < 850)
      q.emit<3>();
    else if (r < 950)
      q.emit<30>();
    else if (r < 952)
      q.emit<2100>();
    else if (r < 980)
      q.other(next() % (r < 979 ? 512 : 2 * Queue::kBlock) + 1);
    else if (r < 990)
      q.commit();
    else if (q.coherent + 1 < q.ready)
      q.complete();
    if (failures)
      break;
  }
}

// --- bench ----------------------------------------------------------------------------------------

struct BenchChunk {
  CommandList<Ctx> list;
};

struct BenchQueue {
  RingBumpState<HostBufferBlockAllocator, 0x400000, null_mutex> ring{{}};
  std::array<BenchChunk, 128> chunks;
  std::atomic_uint64_t ready = 1;
  uint64_t coherent = 0;
  uint32_t count;
  gw2::EmitWindow<BenchChunk> window;

  explicit BenchQueue(uint32_t n) : count(n) {}

  BenchChunk *
  CurrentChunk() {
    return &chunks[ready.load(std::memory_order_relaxed) % count];
  }
  void *
  AllocateCommandData(size_t size, size_t alignment) {
    auto [block, offset] = ring.allocate(ready, coherent, size, alignment);
    return static_cast<char *>(block.ptr) + offset;
  }
  // CommandChunk::allocate_cpu_heap is out of line (dxmt_command_queue.cpp)
  [[gnu::noinline]] void *
  allocateCpuHeap(size_t size, size_t alignment) {
    return AllocateCommandData(size, alignment);
  }
  // before #148: CurrentChunk()->emitcc(fn)
  template <typename F>
  void
  emitcc(F &&fn) {
    BenchChunk *chunk = CurrentChunk();
    chunk->list.emit(std::forward<F>(fn), allocateCpuHeap(sizeof(impl::LambdaCommand<Ctx, F>), 16));
  }
  // CommandQueue::Emit
  template <typename F>
  void
  emit(F &&fn) {
    auto [chunk, ptr] = window.place(*this, sizeof(impl::LambdaCommand<Ctx, F>), 16);
    chunk->list.emit(std::forward<F>(fn), ptr);
  }
  void
  commit() {
    window.reset();
    CurrentChunk()->list.reset();
    coherent = ready.fetch_add(1);
    ring.free_blocks(coherent);
  }
};

static int64_t
now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static void
bench() {
  static BenchQueue q(32);
  constexpr int per_chunk = 60000, rounds = 200;
  uint64_t a = 1, b = 2, c = 3;
  double best[2] = {1e30, 1e30};
  for (int rep = 0; rep < 5; rep++) {
    int64_t spent[2] = {0, 0};
    for (int r = 0; r < rounds; r++) {
      int64_t t0 = now();
      for (int i = 0; i < per_chunk; i++)
        q.emitcc([a, b, c](Ctx &x) { x.next += a + b + c; });
      int64_t t1 = now();
      q.commit();
      int64_t t2 = now();
      for (int i = 0; i < per_chunk; i++)
        q.emit([a, b, c](Ctx &x) { x.next += a + b + c; });
      int64_t t3 = now();
      q.commit();
      spent[0] += t1 - t0;
      spent[1] += t3 - t2;
    }
    double total = double(rounds) * per_chunk;
    best[0] = std::min(best[0], spent[0] / total);
    best[1] = std::min(best[1], spent[1] / total);
  }
  std::printf("emit_window bench: ring per command %.2f ns/emit, window %.2f ns/emit\n", best[0], best[1]);
}

int
main() {
  commitMidRun();
  reuseAfterCompletion(true);
  reuseAfterCompletion(false);
  oversized();
  randomRun();
  bench();
  std::printf("emit_window: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
