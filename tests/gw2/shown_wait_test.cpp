// gw2-test: pure
// gw2-build: clang++ -std=c++20 -O2 -Wall -Wextra -Isrc/dxmt tests/gw2/shown_wait_test.cpp -o $OUT
//
// gw2 (#271): DXMT_MTL4_SHOWN_WAIT (src/dxmt/dxmt_gw2_shown_wait.hpp). No Metal, no Wine, no display.
//
// The encoding context needs the PE toolchain and Metal, so the test plays its two halves around the
// real ShownWait, as src/dxmt/dxmt_context.cpp runs them:
// - record: present() calls presented() with the texture it reads (only with `write`); every texture
//   access with ResourceAccess::Write (gw2NoteShownWrite, in both ArgumentEncodingContext::access
//   overloads for textures) flags its encoder when written() says so. A merge passes the flag on.
// - flush: before a flagged encoder that isn't Null, waitBefore(); after a Metal 3 present, the wait
//   right away with `next` or when presented() said so, else flushed(value, false); a present
//   without a Metal 3 command buffer calls waitBefore() too.
//
// Part 1, where the wait goes:
// - `next`: right after every present, before any of the next frame.
// - `write` (the default since #332): a frame that writes only offscreen textures doesn't wait; each kind of write to the
//   presented texture waits right before its encoder, once. The kinds are the encoders whose
//   destination goes through ArgumentEncodingContext::access with ResourceAccess::Write: a render
//   target (render pass attachments), a clear (clearColor, clearDepthStencil, the UAV clears), a blit
//   (CopyResource and CopySubresourceRegion: CopyTextureBitcast, CopyTextureFromCompressed,
//   CopyTextureToCompressed), UpdateSubresource (UpdateTexture's blit), a UAV write (compute or pixel),
//   a resolve (resolveTexture, or merged into a render pass) and a MetalFX upscale's output. Map can't
//   write the back buffer: it is D3D11_USAGE_DEFAULT, and Map takes only dynamic and staging resources.
// - A read of the presented texture doesn't wait. A merged-away encoder hands the wait to the one it
//   merged into. A write after the present in the same chunk waits for that present. A second
//   texture (a second swap chain or a resize) waits right after its present, and a present that falls
//   back to the Metal 4 way (no Metal 3 command buffer) places a deferred wait at once.
//
// Part 2, the queue arithmetic (#271's acceptance test; the runner presents to no display): 600
// frames at 60 Hz on the GPU, 13.6 ms each (0.2's median), every 10th 18.0 ms, the last 2 ms of each
// writing the back buffer. The Metal 4 queue runs encoders in order and stops at a wait for `shown`;
// the Metal 3 queue runs each present pass (0.2 ms) once the frame's work is done, its drawable is
// free and the pass before it has run. Three drawables. Flip (0.2, PRESENT inuse): a drawable is free
// once the frame shown after its own has reached the screen. Copy (0.1.3): once its own frame has.
// A pass that ends by 1 ms after a vsync is on screen two refreshes after that vsync. These timings
// are my model, not measurements; they give aftergpu 33 ms for flip and 50 ms for copy, where the logs
// read 32.2 (0.2) and 48.3 (0.1.3).
// - flip, `next`: every long frame skips a refresh (on 0.2, 34 of the 38 one-second windows with a GPU
//   frame over 16.7 ms dipped). flip, `write`: none skips, also with 12 ms of back-buffer writes.
// - copy, `next`: none skips (0.1.3). Frames that fit in a refresh skip nothing either way.
// - `write` gives a frame one more refresh, no more: a 35 ms frame still skips.
//
// Part 3, gw2 (#316): ShownRepair, the CPU's signal for a failed Metal 3 present (Mtl4Queue::waitMetal3),
// against a scripted event that can play the GPU's signal of a later present between the CPU's read
// and its set. No failure makes no call on the event; a failure raises `shown`; a race that lowers it
// is repaired by the next present, which ends with `shown` at the highest value.
//
// Run: tests/gw2/run.zsh pure, or the command above with $OUT set.

#include "dxmt_gw2_shown_wait.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <utility>
#include <vector>

using dxmt::gw2::ShownRepair;
using dxmt::gw2::ShownWait;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// textures, by their allocation's address
static int g_textures[4];
static const void *const BB = &g_textures[0];  // the back buffer
static const void *const OFF = &g_textures[1]; // offscreen targets
static const void *const OFF2 = &g_textures[2];
static const void *const BB2 = &g_textures[3]; // a second swap chain's

// One chunk's encoders and the queue's waits, the way the context records and flushes them
class Context {
public:
  explicit Context(bool write) : write_(write) {}

  // an encoder that reads and writes these textures; returns its index in the chunk
  unsigned
  encoder(std::initializer_list<const void *> writes, std::initializer_list<const void *> reads = {}) {
    Enc e;
    for (auto *w : writes)
      if (shown_.source() != nullptr && shown_.written(w)) // gw2NoteShownWrite
        e.flag = true;
    (void)reads; // a read passes through access without ResourceAccess::Write: no flag
    chunk_.push_back(e);
    return unsigned(chunk_.size() - 1);
  }

  // fallback: the Metal 3 command buffer couldn't be made, and it presents the Metal 4 way
  unsigned
  present(const void *texture, bool fallback = false) {
    Enc e;
    e.present = true;
    e.fallback = fallback;
    if (write_)
      e.now = shown_.presented(texture);
    chunk_.push_back(e);
    return unsigned(chunk_.size() - 1);
  }

  // the optimizer merged encoder `from` into `into` (Clear into Render, Render into Render, Resolve into
  // the Render before it): `from` is Null, and its flag goes with it
  void
  merge(unsigned from, unsigned into) {
    chunk_[into].flag |= chunk_[from].flag;
    chunk_[from].null = true;
  }

  struct Wait {
    unsigned before; // the chunk's encoder the wait precedes (the chunk's size: after its last)
    uint64_t value;  // the present's `shown` value it waits for
  };

  std::vector<Wait>
  flush() {
    std::vector<Wait> waits;
    for (unsigned i = 0; i < chunk_.size(); i++) {
      auto &e = chunk_[i];
      if (e.flag && !e.null)
        if (uint64_t v = shown_.waitBefore())
          waits.push_back({i, v});
      if (e.present && e.fallback) {
        // no `shown` of its own; a present left to a write waits now (its texture may not be the
        // source any more)
        if (write_)
          if (uint64_t v = shown_.waitBefore())
            waits.push_back({i, v});
        continue;
      }
      if (e.present) {
        uint64_t value = ++values_;
        bool now = !write_ || e.now;
        if (now)
          waits.push_back({i + 1, value});
        if (write_)
          shown_.flushed(value, now);
      }
    }
    chunk_.clear();
    return waits;
  }

private:
  struct Enc {
    bool flag = false, present = false, now = true, null = false, fallback = false;
  };
  bool write_;
  ShownWait shown_;
  std::vector<Enc> chunk_;
  uint64_t values_ = 0;
};

// two frames: the first present waits right away; the second frame renders only offscreen, so its
// present's wait (`shown` value 2) is still to be placed
static void
twoFrames(Context &ctx) {
  ctx.encoder({OFF});
  ctx.encoder({BB}, {OFF});
  ctx.present(BB);
  ctx.flush();
  ctx.encoder({OFF});
  ctx.present(BB);
  ctx.flush();
}

static void
placement() {
  // `next`: the wait right after the present, before the next frame's first encoder
  {
    Context ctx(false);
    ctx.encoder({OFF});
    ctx.encoder({BB});
    unsigned p = ctx.present(BB);
    auto w = ctx.flush();
    CHECK(w.size() == 1 && w[0].before == p + 1 && w[0].value == 1);
    ctx.encoder({OFF});
    ctx.encoder({BB});
    ctx.present(BB);
    w = ctx.flush();
    CHECK(w.size() == 1 && w[0].value == 2); // after its present again, none before the writes
  }
  // `write`: the first present waits right away (nothing before it to compare); then an offscreen-only
  // frame doesn't wait at all
  {
    Context ctx(true);
    ctx.encoder({OFF});
    unsigned p = ctx.present(BB);
    auto w = ctx.flush();
    CHECK(w.size() == 1 && w[0].before == p + 1);
    ctx.encoder({BB}); // frame 2 writes the back buffer: present 1 was waited for already
    ctx.present(BB);
    w = ctx.flush();
    CHECK(w.empty());
    ctx.encoder({OFF});
    ctx.encoder({OFF2}, {OFF});
    ctx.encoder({OFF}, {BB}); // reads the presented texture: no wait
    ctx.present(BB);          // presents the unchanged back buffer
    w = ctx.flush();
    CHECK(w.empty());
    // the frame after: its first back-buffer write waits for the newest present (3)
    ctx.encoder({OFF});
    unsigned i = ctx.encoder({BB});
    ctx.present(BB);
    w = ctx.flush();
    CHECK(w.size() == 1 && w[0].before == i && w[0].value == 3);
  }
  // every kind of write to the back buffer waits right before its encoder, once. In the context they
  // differ in the encoder and in what else it names; each names the back buffer with ResourceAccess::Write
  struct Kind {
    const char *name;
    std::initializer_list<const void *> writes, reads;
  };
  const Kind kinds[] = {
      {"render target (a render pass's color attachment)", {BB, OFF2}, {OFF}},
      {"ClearRenderTargetView (clearColor)", {BB}, {}},
      {"ClearDepthStencilView (clearDepthStencil)", {BB}, {}},
      {"ClearUnorderedAccessView (the compute clear's texture)", {BB}, {}},
      {"CopyResource (a blit, CopyTextureBitcast)", {BB}, {OFF}},
      {"CopySubresourceRegion (a blit, from or to a compressed format)", {BB}, {OFF2}},
      {"UpdateSubresource (UpdateTexture's blit from a staging buffer)", {BB}, {}},
      {"a compute shader's UAV", {BB, OFF}, {OFF2}},
      {"a pixel shader's UAV (render pass)", {OFF, BB}, {OFF2}},
      {"ResolveSubresource (resolveTexture)", {BB}, {OFF}},
      {"a MetalFX upscale's output", {BB}, {OFF}},
  };
  for (auto &kind : kinds) {
    Context ctx(true);
    twoFrames(ctx);
    ctx.encoder({OFF});
    ctx.encoder({OFF2}, {OFF});
    unsigned i = ctx.encoder(kind.writes, kind.reads);
    unsigned j = ctx.encoder({BB}); // a second write: the queue already waits
    ctx.present(BB);
    auto w = ctx.flush();
    bool ok = w.size() == 1 && w[0].before == i && w[0].value == 2;
    if (!ok)
      std::printf("FAIL %s: waits=%zu before=%u (want one, before %u, not %u)\n", kind.name, w.size(),
                  w.empty() ? 0u : w[0].before, i, j);
    CHECK(ok);
  }
  // a clear of the back buffer merged into the render pass after it: the wait goes with it
  {
    Context ctx(true);
    twoFrames(ctx);
    ctx.encoder({OFF});
    unsigned clear = ctx.encoder({BB});
    ctx.encoder({OFF2});
    unsigned render = ctx.encoder({BB});
    ctx.merge(clear, render);
    ctx.present(BB);
    auto w = ctx.flush();
    CHECK(w.size() == 1 && w[0].before == render);
  }
  // a resolve into the back buffer merged into the render pass before it: the wait moves up to it
  {
    Context ctx(true);
    twoFrames(ctx);
    unsigned render = ctx.encoder({OFF});
    unsigned resolve = ctx.encoder({BB}, {OFF});
    ctx.merge(resolve, render);
    ctx.present(BB);
    auto w = ctx.flush();
    CHECK(w.size() == 1 && w[0].before == render);
  }
  // a write after the present in the same chunk waits for that present
  {
    Context ctx(true);
    twoFrames(ctx);
    ctx.encoder({BB}); // waits for present 2
    ctx.present(BB);   // present 3, deferred
    unsigned after = ctx.encoder({BB});
    auto w = ctx.flush();
    CHECK(w.size() == 2 && w[0].value == 2 && w[1].before == after && w[1].value == 3);
  }
  // a second swap chain's present that falls back to the Metal 4 way: the first texture's deferred
  // wait is placed at once, since a write to it isn't flagged any more
  {
    Context ctx(true);
    twoFrames(ctx);
    unsigned p = ctx.present(BB2, true);
    auto w = ctx.flush();
    CHECK(w.size() == 1 && w[0].before == p && w[0].value == 2);
    ctx.encoder({BB}); // after the wait for present 2
    w = ctx.flush();
    CHECK(w.empty());
  }
  // a second swap chain: its present waits right away, and a write to the first texture is behind it
  {
    Context ctx(true);
    twoFrames(ctx);
    unsigned p = ctx.present(BB2);
    auto w = ctx.flush();
    CHECK(w.size() == 1 && w[0].before == p + 1 && w[0].value == 3);
    ctx.encoder({BB}); // the first texture: not the source any more, and present 3 is waited for
    w = ctx.flush();
    CHECK(w.empty());
    ctx.encoder({BB2}); // the second's: waited for already
    w = ctx.flush();
    CHECK(w.empty());
  }
}

// ---- part 2: the queue arithmetic

constexpr double kRefresh = 1000.0 / 60.0;
constexpr double kLatch = 1.0; // a pass ending this long after a vsync still makes it
constexpr double kPass = 0.2;

struct Run {
  int skipped = 0;
  double aftergpu = 0; // ms from the pass's end to the screen, frames 100 and later
};

static Run
simulate(bool write, bool flip, double long_ms, double bb_ms, int frames = 600, int every = 10) {
  Context ctx(write);
  std::vector<double> pass_end;
  std::vector<long> shown; // the vsync each frame reached the screen at
  double gpu = 0;          // the Metal 4 queue's time
  std::vector<Context::Wait> carried; // waits after the last chunk's present: before this chunk
  Run run;
  int counted = 0;
  for (int n = 0; n < frames; n++) {
    double work = n % every == every - 1 ? long_ms : 13.6;
    ctx.encoder({OFF});
    ctx.encoder({BB}, {OFF});
    ctx.present(BB);
    auto waits = ctx.flush();
    double dur[2] = {work - bb_ms, bb_ms};
    double t = gpu;
    for (unsigned i = 0; i < 2; i++) {
      auto wait_here = [&](const Context::Wait &w) { t = std::max(t, pass_end[w.value - 1]); };
      if (i == 0)
        for (auto &w : carried)
          wait_here(w);
      for (auto &w : waits)
        if (w.before == i)
          wait_here(w);
      t += dur[i];
    }
    gpu = t;
    carried.clear();
    for (auto &w : waits)
      if (w.before == 3) // after the present: before the next chunk
        carried.push_back(w);
    // the Metal 3 queue
    double free = 0;
    if (n >= 3)
      free = (flip ? shown[n - 2] : shown[n - 3]) * kRefresh;
    double start = std::max({gpu, free, n ? pass_end[n - 1] : 0.0});
    pass_end.push_back(start + kPass);
    long s = long(std::ceil((pass_end[n] - kLatch) / kRefresh)) + 2;
    if (n) {
      s = std::max(s, shown[n - 1] + 1);
      run.skipped += int(s - shown[n - 1] - 1);
    }
    shown.push_back(s);
    if (n >= 100) {
      run.aftergpu += s * kRefresh - pass_end[n];
      counted++;
    }
  }
  run.aftergpu /= counted;
  return run;
}

static void
queueArithmetic() {
  Run flip_next = simulate(false, true, 18.0, 2.0);
  Run flip_write = simulate(true, true, 18.0, 2.0);
  Run flip_write_late = simulate(true, true, 18.0, 12.0);
  Run copy_next = simulate(false, false, 18.0, 2.0);
  Run fit_next = simulate(false, true, 13.6, 2.0);
  Run fit_write = simulate(true, true, 13.6, 2.0);
  Run flip_write_35 = simulate(true, true, 35.0, 2.0);
  CHECK(flip_next.skipped == 60); // one per long frame
  CHECK(flip_write.skipped == 0);
  CHECK(flip_write_late.skipped == 0);
  CHECK(copy_next.skipped == 0);
  CHECK(fit_next.skipped == 0 && fit_write.skipped == 0);
  CHECK(flip_write_35.skipped > 0);
  // the model's steady state against the logs: about two refreshes with flip, three with copy
  CHECK(std::fabs(fit_next.aftergpu - 2 * kRefresh) < 1);
  CHECK(std::fabs(copy_next.aftergpu - 3 * kRefresh) < 1);
  std::printf("RESULT skipped of 600 (60 frames of 18 ms): flip next=%d write=%d write(bb 12 ms)=%d; copy next=%d; "
              "35 ms frames, write=%d; aftergpu flip=%.1f copy=%.1f\n",
              flip_next.skipped, flip_write.skipped, flip_write_late.skipped, copy_next.skipped, flip_write_35.skipped,
              fit_next.aftergpu, copy_next.aftergpu);
}

// gw2 (#316): `shown` as Mtl4Queue sees it through the bridge. after_read, if set, runs once right after
// a signaledValue() read: the GPU signalling a later present before the CPU's set.
struct ScriptedEvent {
  uint64_t value = 0;
  int calls = 0;
  std::function<void()> after_read;

  uint64_t
  signaledValue() {
    calls++;
    uint64_t v = value;
    if (after_read)
      std::exchange(after_read, nullptr)();
    return v;
  }
  void
  signalValue(uint64_t v) {
    calls++;
    value = v;
  }
};

static void
shownRepair() {
  {
    // no failure ever: no call on the event
    ShownRepair repair;
    ScriptedEvent event;
    for (uint64_t n = 1; n <= 100; n++)
      CHECK(!repair.completed(event, n, false));
    CHECK(event.calls == 0);
  }
  {
    // a failure with no race: the CPU signals in the present's place
    ShownRepair repair;
    ScriptedEvent event;
    event.value = 3;
    CHECK(repair.completed(event, 5, true));
    CHECK(event.value == 5);
  }
  {
    // a failure after the GPU had already signalled further: nothing set, and nothing to repair later
    ShownRepair repair;
    ScriptedEvent event;
    event.value = 7;
    CHECK(!repair.completed(event, 5, true));
    CHECK(event.value == 7);
    int calls = event.calls;
    CHECK(!repair.completed(event, 8, false));
    CHECK(event.calls == calls);
  }
  {
    // the race: present 9's GPU signal lands between the read and the set, which lowers `shown` to 5;
    // present 9 completes next and raises it back
    ShownRepair repair;
    ScriptedEvent event;
    event.value = 3;
    event.after_read = [&] { event.value = 9; };
    CHECK(repair.completed(event, 5, true));
    CHECK(event.value == 5);
    CHECK(repair.completed(event, 9, false));
    CHECK(event.value == 9);
    // present 10 signalled on the GPU: nothing to raise
    event.value = 10;
    CHECK(!repair.completed(event, 10, false));
    CHECK(event.value == 10);
  }
}

int
main() {
  placement();
  queueArithmetic();
  shownRepair();
  if (failures) {
    std::printf("shown_wait_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("shown_wait_test: passed\n");
  return 0;
}
