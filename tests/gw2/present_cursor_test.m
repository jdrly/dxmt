// gw2-test: display
// gw2-build: for arch in arm64 x86_64; do
// gw2-build:   clang -x objective-c -std=gnu17 -fobjc-arc -O1 -Wall -Werror -arch $arch -mmacosx-version-min=26.0 tests/gw2/present_cursor_test.m -framework Cocoa -framework Metal -framework QuartzCore -framework IOSurface -framework CoreGraphics -o ${OUT}_$arch
// gw2-build:   codesign -s - -f ${OUT}_$arch
// gw2-build: done
// gw2-run: for arch in arm64 x86_64; do
// gw2-run:   for v in "--present mtl3" "--present mtl4" "--present mtl4 --layer-set queue" "--present mtl4 --empty-commit off" "--present mtl4 --present-thread queue"; do
// gw2-run:     MTL_DEBUG_LAYER=1 ${OUT}_$arch ${=v} --pointer still
// gw2-run:   done
// gw2-run: done
//
// gw2 (#225, for #223): on the arm64 engine with Metal 4, moving the cursor makes every frame a
// refresh late (40 fps at 60 Hz with three drawables), and with Metal 3 it doesn't. This is the
// smallest program I could write that might show the same without Wine, FEX, DXMT or the game: one
// borderless full-screen window, one CAMetalLayer set up as my presenter sets it up, a frame with a
// GPU cost I can dial, and the present done the Metal 3 way or the Metal 4 way, while a thread moves
// the pointer. It changes nothing in the driver.
//
// It isn't part of `tests/gw2/run.zsh all`: it needs a logged-in GUI session, a display, the pointer,
// and for --present mtl4 a Metal 4 device. `tests/gw2/run.zsh display` builds it and runs the
// acceptance runs above (both ways and each Metal 4 variant, 600 frames, MTL_DEBUG_LAYER=1, arm64 and
// x86_64, pointer still). GitHub's runner only builds it (`run.zsh display --build-only`). By hand:
//
//   tests/gw2/run.zsh display --build-only     # prints the logs directory; the binaries are in it
//   $T/present_cursor_test_arm64 --present mtl4 --pointer wave
//   $T/present_cursor_test_arm64 --present mtl3 --pointer wave
//   sudo sample WindowServer 10 10 -file ~/Desktop/ws-mtl4-wave.txt     # beside a run, from another terminal
//
// Run it from a terminal inside the GUI session, with the game closed and nothing over the window.
//
// What it sets up, from my fork (src/dxmt/dxmt_presenter.cpp's constructor, src/winemetal/unix/
// winemetal_unix.c's _MetalLayer_setProps and _MetalLayer_setColorSpace): BGRA8Unorm, framebufferOnly
// off, display sync on, opaque, colorspace sRGB without EDR, maximumDrawableCount left at its 3. The
// layer is the window's content view's own layer at the screen's backing size, and the frame view's
// background colour is cleared, as in athei's probe (athei/mtld3d #1123), which went direct on an
// M1 Max and an M5 Max. The window floats above the menu bar, with the Dock and menu bar hidden.
//
// Each frame (three in flight, DXMT's default frame latency):
// 1. The frame pass: a fragment loop into an offscreen BGRA8 texture at the drawable's size. At start
//    the loop's length is calibrated on a Metal 3 queue to --gpu-ms (default 13 ms), so the drawable
//    pool fills at 60 because the GPU is busy, as in the game. Its command buffer is committed on
//    its own (my fork's DXMT_PRESENT_SPLIT, on by default).
// 2. nextDrawable, timed.
// 3. The present pass: a full-screen triangle that copies the offscreen texture into the drawable,
//    as DXMT's present pass copies the back buffer.
//    --present mtl3 (dxmt_context.cpp's Metal 3 branch): a command buffer of its own, then
//      presentDrawable:atTime: one refresh ahead, then commit. Metal's hazard tracking orders it.
//    --present mtl4 (its Metal 4 branch, dxmt_presenter.cpp's encodeCommandsMetal4, mtl4.c):
//      waitForDrawable right after nextDrawable; a command buffer for the pass, with the layer's
//      residency set on it; the pass waits on a fence the frame pass updated, and updates one the next
//      frame pass waits on; commit with feedback; signalDrawable; [drawable presentAtTime:] one refresh
//      ahead from the encoding thread; then an empty command buffer committed with feedback, as a GW2
//      frame gets with DXMT_MTL4_TIMESTAMP_REUSE off. The queue holds a residency set of its own with
//      the offscreen texture, the cost buffer and both pipelines (DXMT's device set, #29).
//    Metal 4 variants, one flag each:
//      --layer-set queue         the layer's set on the queue for the whole run, as before #184
//      --empty-commit off        no empty command buffer after the present (DXMT_MTL4_TIMESTAMP_REUSE=1)
//      --present-thread queue    presentAtTime: from a serial dispatch queue, not the encoding thread
// --pointer wave moves the pointer in a circle (a quarter of the screen's height) 80 times a second,
// one turn a second, with CGWarpMouseCursorPosition, or with posted mouse-moved events with
// --move event (needs the Accessibility permission for the terminal). Whether a warp makes the window
// server update its cursor the way a mouse does I haven't checked; the start line says which way it
// used. The pointer goes back where it was at the end, also after Ctrl-C or SIGTERM.
//
// Output. At start: the process's architecture (Rosetta or not), the device, the display (size,
// refresh rate, built-in or external, vendor and model), the layer, the calibrated cost. Then one line
// a second, for the frames whose GPU work and presented handler have come back:
//   frames, dropped    frames taken; dropped: not presented (presentedTime 0, or no drawable)
//   interval p50/p95   presentedTime minus the last presented frame's, ms. 16.7 is flat at 60 Hz
//   missed             refreshes skipped between presented frames (an interval of two refreshes is 1)
//   shown p50/p95      presentedTime minus the time nextDrawable returned, ms
//   nextdrawable       how long nextDrawable blocked, ms
//   passwait           the present pass's GPU start minus its commit, ms: how long Metal held the pass
//   early n/m          of the m frames whose drawable's surface had been on screen before, n started
//                      their present pass before the frame after that surface's last one was presented,
//                      so while that surface may still have been on screen (by the layer's own clock)
//   gpu p50            the frame pass's GPU time, ms
//   moves              pointer moves in that second
//   path               the layer's private displayCompositingInternalStatus, read on the main thread if
//                      the layer responds to it: 0 is direct (athei found it 0 whenever the Metal HUD
//                      said Direct), anything else is composited. A composited line says so.
// At the end, one summary line over the frames after the first second, and, if any second was
// composited or unknown, a line that says this run can't show #223.
//
// What it can't tell: whether the cursor is a hardware plane (no public read-out; the window server's
// sample shows cursor_callback and set_cursor_surface), and a flip from a copy on the direct path (the
// status is 0 for both, by athei's reading; the window server's sample shows copy_surface).
//
// Exit: 0 when every frame was encoded and no command buffer reported an error; 1 on a Metal error or
// if no frame was presented; 2 on a bad argument; 77 (skipped) without a GUI session, without a
// display, without a Metal device, without Metal 4 for --present mtl4, without the permission for
// --move event, or when the layer gives no drawable three times in a row (the window isn't on a
// display); 130 after Ctrl-C.

#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <objc/message.h>
#include <os/lock.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

enum { kInFlight = 3, kRing = 1024, kCostStride = 256, kSurfaces = 16, kPointerHz = 80 };

static const char *kShaders =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut vs_full(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
    "  VOut o; o.pos = float4(p * 2.0 - 1.0, 0, 1); return o;\n"
    "}\n"
    "struct Cost { uint iterations; float phase; };\n"
    "fragment float4 fs_cost(VOut in [[stage_in]], constant Cost &c [[buffer(0)]]) {\n"
    "  float x = in.pos.x * 0.00071 + in.pos.y * 0.00037 + c.phase;\n"
    "  for (uint i = 0; i < c.iterations; i++) x = fract(x * 1.6180339 + 0.1234567);\n"
    "  return float4(x, fract(in.pos.y / 256.0 + c.phase), 0.5, 1);\n"
    "}\n"
    "fragment float4 fs_copy(VOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {\n"
    "  return src.read(uint2(in.pos.xy));\n"
    "}\n";

typedef struct {
  uint32_t iterations;
  float phase;
} Cost;

static struct {
  bool mtl4, set_on_queue, empty_commit, present_on_queue, wave, move_event;
  long frames;
  double gpu_ms;
} opt = {.mtl4 = true, .empty_commit = true, .frames = 600, .gpu_ms = 13};

// --- what the threads share

typedef struct {
  uint64_t frame;
  double take, next_wait, pass_commit, pass_start, gpu_cost, presented;
  uint32_t surface;
  bool gpu_done, shown_done, no_drawable;
} Frame;

static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT; // g_ring and the report state
static Frame g_ring[kRing];
static _Atomic int g_pending[kRing];
static _Atomic uint64_t g_taken; // frames fully encoded
static _Atomic int g_errors;
static _Atomic unsigned g_moves;
static _Atomic bool g_pointer_stop;
static double g_period = 1.0 / 60; // one refresh, s

static id<MTLDevice> g_device;
static CAMetalLayer *g_layer;
static id<MTLRenderPipelineState> g_cost_pso, g_copy_pso;
static id<MTLTexture> g_offscreen;
static id<MTLBuffer> g_costbuf;
static uint32_t g_iterations;
static dispatch_semaphore_t g_slot[kInFlight];
// Metal 3
static id<MTLCommandQueue> g_q3;
// Metal 4
static id<MTL4CommandQueue> g_q4;
static id<MTL4CommandAllocator> g_alloc[kInFlight];
static id<MTL4CommandBuffer> g_fcb[kInFlight], g_pcb[kInFlight], g_ecb[kInFlight];
static id<MTL4ArgumentTable> g_cost_table, g_copy_table;
static id<MTLFence> g_raw, g_war; // frame pass -> present pass, present pass -> next frame pass
static bool g_war_updated;
static id<MTLResidencySet> g_layer_set;
static dispatch_queue_t g_present_queue;

static Frame *
frame_at(uint64_t k) {
  return &g_ring[k % kRing];
}

static void
gpu_error(const char *what, NSError *error) {
  if (atomic_fetch_add(&g_errors, 1) < 5)
    printf("Metal error (%s): %s\n", what, error.description.UTF8String);
}

// a frame is done on the GPU when every command buffer it committed has come back; the frame itself
// holds one reference until its encoding ends
static void
frame_ref(uint64_t k) {
  atomic_fetch_add(&g_pending[k % kRing], 1);
}

static void
frame_unref(uint64_t k) {
  if (atomic_fetch_sub(&g_pending[k % kRing], 1) != 1)
    return;
  os_unfair_lock_lock(&g_lock);
  if (frame_at(k)->frame == k)
    frame_at(k)->gpu_done = true;
  os_unfair_lock_unlock(&g_lock);
  dispatch_semaphore_signal(g_slot[k % kInFlight]);
}

#define SET(k, field, value)                                                                       \
  do {                                                                                             \
    os_unfair_lock_lock(&g_lock);                                                                  \
    if (frame_at(k)->frame == (k))                                                                 \
      frame_at(k)->field = (value);                                                                \
    os_unfair_lock_unlock(&g_lock);                                                                \
  } while (0)

// --- the pointer

static pthread_t g_pointer_thread;
static bool g_pointer_started;
static CGPoint g_pointer_home;

static void *
pointer_main(void *arg) {
  (void)arg;
  CGRect b = CGDisplayBounds(CGMainDisplayID());
  CGPoint c = CGPointMake(CGRectGetMidX(b), CGRectGetMidY(b));
  double r = b.size.height / 4;
  uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  for (uint64_t i = 0; !atomic_load(&g_pointer_stop); i++) {
    double a = 2 * M_PI * (double)(i % kPointerHz) / kPointerHz;
    CGPoint p = CGPointMake(c.x + r * cos(a), c.y + r * sin(a));
    if (opt.move_event) {
      CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, kCGMouseButtonLeft);
      CGEventPost(kCGHIDEventTap, e);
      CFRelease(e);
    } else {
      CGWarpMouseCursorPosition(p);
    }
    atomic_fetch_add(&g_moves, 1);
    uint64_t next = start + (i + 1) * 1000000000ull / kPointerHz;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (next > now) {
      struct timespec ts = {(time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull)};
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

static void
pointer_start(void) {
  CGEventRef e = CGEventCreate(NULL);
  g_pointer_home = CGEventGetLocation(e);
  CFRelease(e);
  if (pthread_create(&g_pointer_thread, NULL, pointer_main, NULL) == 0)
    g_pointer_started = true;
}

// stops the pointer thread and puts the pointer back; main thread only
static void
pointer_stop(void) {
  if (!g_pointer_started)
    return;
  g_pointer_started = false;
  atomic_store(&g_pointer_stop, true);
  pthread_join(g_pointer_thread, NULL);
  CGWarpMouseCursorPosition(g_pointer_home);
  printf("pointer: back at %.0f,%.0f\n", g_pointer_home.x, g_pointer_home.y);
}

// --- the frame

// nextDrawable, timed; the presented handler goes on before any present
static id<CAMetalDrawable>
take(uint64_t k) {
  double t0 = CACurrentMediaTime();
  id<CAMetalDrawable> d = [g_layer nextDrawable];
  double t1 = CACurrentMediaTime();
  os_unfair_lock_lock(&g_lock);
  frame_at(k)->take = t1;
  frame_at(k)->next_wait = t1 - t0;
  if (!d) {
    frame_at(k)->no_drawable = true;
    frame_at(k)->shown_done = true;
  } else {
    IOSurfaceRef surface = d.texture.iosurface;
    frame_at(k)->surface = surface ? IOSurfaceGetID(surface) : 0;
  }
  os_unfair_lock_unlock(&g_lock);
  if (d)
    [d addPresentedHandler:^(id<MTLDrawable> shown) {
      double t = shown.presentedTime;
      os_unfair_lock_lock(&g_lock);
      if (frame_at(k)->frame == k) {
        frame_at(k)->presented = t;
        frame_at(k)->shown_done = true;
      }
      os_unfair_lock_unlock(&g_lock);
    }];
  return d;
}

static void
frame3(uint64_t k, unsigned s) {
  id<MTLCommandBuffer> fcb = [g_q3 commandBuffer];
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = g_offscreen;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> e = [fcb renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:g_cost_pso];
  [e setFragmentBuffer:g_costbuf offset:s * kCostStride atIndex:0];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e endEncoding];
  frame_ref(k);
  [fcb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
    if (cb.error)
      gpu_error("frame pass", cb.error);
    SET(k, gpu_cost, cb.GPUEndTime - cb.GPUStartTime);
    frame_unref(k);
  }];
  [fcb commit];

  id<CAMetalDrawable> d = take(k);
  if (!d)
    return;
  id<MTLCommandBuffer> pcb = [g_q3 commandBuffer];
  rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = d.texture;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  e = [pcb renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:g_copy_pso];
  [e setFragmentTexture:g_offscreen atIndex:0];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e endEncoding];
  [pcb presentDrawable:d atTime:CACurrentMediaTime() + g_period];
  frame_ref(k);
  [pcb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
    if (cb.error)
      gpu_error("present pass", cb.error);
    SET(k, pass_start, cb.GPUStartTime);
    frame_unref(k);
  }];
  SET(k, pass_commit, CACurrentMediaTime());
  [pcb commit];
}

enum { kFramePass, kPresentPass, kEmpty };
static const char *kCommitNames[] = {"frame pass", "present pass", "empty command buffer"};

static void
commit4(uint64_t k, id<MTL4CommandBuffer> cb, int kind) {
  frame_ref(k);
  MTL4CommitOptions *options = [MTL4CommitOptions new]; // one per commit, as mtl4.c does
  [options addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
    if (fb.error)
      gpu_error(kCommitNames[kind], fb.error);
    if (kind == kFramePass)
      SET(k, gpu_cost, fb.GPUEndTime - fb.GPUStartTime);
    else if (kind == kPresentPass)
      SET(k, pass_start, fb.GPUStartTime);
    frame_unref(k);
  }];
  __unsafe_unretained id<MTL4CommandBuffer> list[1] = {cb};
  [g_q4 commit:list count:1 options:options];
}

static void
frame4(uint64_t k, unsigned s) {
  id<MTL4CommandBuffer> fcb = g_fcb[s];
  [fcb beginCommandBufferWithAllocator:g_alloc[s]];
  MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
  rp.colorAttachments[0].texture = g_offscreen;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTL4RenderCommandEncoder> e = [fcb renderCommandEncoderWithDescriptor:rp];
  if (g_war_updated)
    [e waitForFence:g_war beforeEncoderStages:MTLStageFragment];
  [e setRenderPipelineState:g_cost_pso];
  [g_cost_table setAddress:g_costbuf.gpuAddress + s * kCostStride atIndex:0];
  [e setArgumentTable:g_cost_table atStages:MTLRenderStageFragment];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e updateFence:g_raw afterEncoderStages:MTLStageFragment];
  [e endEncoding];
  [fcb endCommandBuffer];
  commit4(k, fcb, kFramePass);

  id<CAMetalDrawable> d = take(k);
  if (!d)
    return;
  [g_q4 waitForDrawable:d];
  id<MTL4CommandBuffer> pcb = g_pcb[s];
  [pcb beginCommandBufferWithAllocator:g_alloc[s]];
  if (!opt.set_on_queue && g_layer_set)
    [pcb useResidencySet:g_layer_set];
  rp = [MTL4RenderPassDescriptor new];
  rp.colorAttachments[0].texture = d.texture;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  e = [pcb renderCommandEncoderWithDescriptor:rp];
  [e waitForFence:g_raw beforeEncoderStages:MTLStageFragment];
  [e setRenderPipelineState:g_copy_pso];
  [g_copy_table setTexture:g_offscreen.gpuResourceID atIndex:0];
  [e setArgumentTable:g_copy_table atStages:MTLRenderStageFragment];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e updateFence:g_war afterEncoderStages:MTLStageFragment];
  [e endEncoding];
  [pcb endCommandBuffer];
  g_war_updated = true;
  SET(k, pass_commit, CACurrentMediaTime());
  commit4(k, pcb, kPresentPass);
  [g_q4 signalDrawable:d];
  if (opt.present_on_queue)
    dispatch_async(g_present_queue, ^{
      [d presentAtTime:CACurrentMediaTime() + g_period];
    });
  else
    [d presentAtTime:CACurrentMediaTime() + g_period];
  if (opt.empty_commit) {
    [g_ecb[s] beginCommandBufferWithAllocator:g_alloc[s]];
    [g_ecb[s] endCommandBuffer];
    commit4(k, g_ecb[s], kEmpty);
  }
}

static void finish(int code);

static void *
render_main(void *arg) {
  (void)arg;
  int no_drawable_run = 0;
  for (uint64_t k = 0; k < (uint64_t)opt.frames; k++) {
    @autoreleasepool {
      unsigned s = k % kInFlight;
      dispatch_semaphore_wait(g_slot[s], DISPATCH_TIME_FOREVER); // frame k - 3 is done on the GPU
      if (opt.mtl4)
        [g_alloc[s] reset];
      os_unfair_lock_lock(&g_lock);
      memset(frame_at(k), 0, sizeof(Frame));
      frame_at(k)->frame = k;
      os_unfair_lock_unlock(&g_lock);
      atomic_store(&g_pending[k % kRing], 1);
      Cost *cost = (Cost *)((char *)g_costbuf.contents + s * kCostStride);
      cost->iterations = g_iterations;
      cost->phase = (float)(k % 600) / 600.0f;
      if (opt.mtl4)
        frame4(k, s);
      else
        frame3(k, s);
      os_unfair_lock_lock(&g_lock);
      bool none = frame_at(k)->no_drawable;
      os_unfair_lock_unlock(&g_lock);
      frame_unref(k);
      atomic_store(&g_taken, k + 1);
      no_drawable_run = none ? no_drawable_run + 1 : 0;
      if (no_drawable_run == 3) {
        dispatch_async(dispatch_get_main_queue(), ^{
          printf("skip: the layer gave no drawable three times in a row: the window isn't on a display\n");
          pointer_stop();
          exit(77);
        });
        return NULL;
      }
    }
  }
  for (unsigned s = 0; s < kInFlight; s++)
    dispatch_semaphore_wait(g_slot[s], DISPATCH_TIME_FOREVER); // everything done on the GPU
  usleep(300000); // the last frames' presented handlers
  dispatch_async(dispatch_get_main_queue(), ^{
    finish(0);
  });
  return NULL;
}

// --- the report

typedef struct {
  double *v;
  size_t n, cap;
} Series;

typedef struct {
  Series interval, shown, next_wait, pass_wait, gpu;
  long frames, dropped, missed, early, early_known;
} Window;

static Window g_win, g_sum;
static uint64_t g_next;        // the next frame to report
static uint64_t g_warm;        // frames in the first second, left out of the summary
static double g_last_presented; // the last presented frame's presentedTime
static uint32_t g_last_surface;
static uint64_t g_last_frame;
static struct {
  uint32_t id;
  uint64_t frame;  // the last frame that took the surface
  double replaced; // when the frame after that one was presented; 0 until then
} g_surf[kSurfaces];
static int g_seconds, g_direct, g_composited, g_unknown;

static void
push(Series *s, double x) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 128;
    s->v = realloc(s->v, s->cap * sizeof(double));
  }
  s->v[s->n++] = x;
}

static int
cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

// ms; NAN when empty (printed as "nan")
static double
pct(Series *s, double p) {
  if (!s->n)
    return NAN;
  qsort(s->v, s->n, sizeof(double), cmp_double);
  return s->v[(size_t)lround(p * (double)(s->n - 1))] * 1e3;
}

static void
window_reset(Window *w) {
  w->interval.n = w->shown.n = w->next_wait.n = w->pass_wait.n = w->gpu.n = 0;
  w->frames = w->dropped = w->missed = w->early = w->early_known = 0;
}

static int
surface_slot(uint32_t id) {
  int free_slot = -1, oldest = 0;
  for (int i = 0; i < kSurfaces; i++) {
    if (g_surf[i].id == id)
      return i;
    if (!g_surf[i].id && free_slot < 0)
      free_slot = i;
    if (g_surf[i].frame < g_surf[oldest].frame)
      oldest = i;
  }
  int i = free_slot >= 0 ? free_slot : oldest;
  g_surf[i].id = id;
  g_surf[i].frame = UINT64_MAX; // not seen yet
  g_surf[i].replaced = 0;
  return i;
}

static void
account(Window *w, const Frame *f, bool early_known, bool early, double interval, long missed) {
  w->frames++;
  push(&w->next_wait, f->next_wait);
  if (f->gpu_cost > 0)
    push(&w->gpu, f->gpu_cost);
  if (f->pass_start > 0 && f->pass_commit > 0)
    push(&w->pass_wait, f->pass_start - f->pass_commit);
  if (early_known) {
    w->early_known++;
    w->early += early;
  }
  if (f->presented > 0) {
    push(&w->shown, f->presented - f->take);
    if (interval > 0) {
      push(&w->interval, interval);
      w->missed += missed;
    }
  } else {
    w->dropped++;
  }
}

// takes the frames that have come back, in order; with `final`, all of them
static void
collect(bool final) {
  uint64_t taken = atomic_load(&g_taken);
  double now = CACurrentMediaTime();
  os_unfair_lock_lock(&g_lock);
  for (; g_next < taken; g_next++) {
    uint64_t k = g_next;
    Frame f = *frame_at(k);
    // a frame whose handlers haven't come back in a second is taken as it is
    if (!(f.gpu_done && f.shown_done) && !final && now - f.take < 1.0)
      break;
    bool early_known = false, early = false;
    if (f.surface && !f.no_drawable) {
      int i = surface_slot(f.surface);
      if (g_surf[i].frame < k && g_surf[i].replaced > 0 && f.pass_start > 0) {
        early_known = true;
        early = f.pass_start < g_surf[i].replaced;
      }
      g_surf[i].frame = k;
      g_surf[i].replaced = 0;
    }
    double interval = 0;
    long missed = 0;
    if (f.presented > 0) {
      if (g_last_presented > 0) {
        interval = f.presented - g_last_presented;
        missed = lround(interval / g_period) - 1;
        if (missed < 0)
          missed = 0;
      }
      // the surface on screen until now is free to show again from here
      if (g_last_surface) {
        int i = surface_slot(g_last_surface);
        if (g_surf[i].frame == g_last_frame)
          g_surf[i].replaced = f.presented;
      }
      g_last_presented = f.presented;
      g_last_surface = f.surface;
      g_last_frame = k;
    }
    account(&g_win, &f, early_known, early, interval, missed);
    if (k >= g_warm)
      account(&g_sum, &f, early_known, early, interval, missed);
  }
  os_unfair_lock_unlock(&g_lock);
}

// -[CAMetalLayer displayCompositingInternalStatus], private; main thread
static void
path_text(char *out, size_t size) {
  SEL sel = sel_registerName("displayCompositingInternalStatus");
  if (![g_layer respondsToSelector:sel]) {
    g_unknown++;
    snprintf(out, size, "path=unknown (the layer has no displayCompositingInternalStatus)");
    return;
  }
  unsigned long long status = ((unsigned long long (*)(id, SEL))objc_msgSend)(g_layer, sel);
  if (status == 0) {
    g_direct++;
    snprintf(out, size, "path=direct status=0x0");
  } else {
    g_composited++;
    snprintf(out, size, "path=COMPOSITED status=0x%llx: not the direct path, this run can't show #223", status);
  }
}

static void
print_window(const char *label, Window *w, const char *path) {
  printf(
      "%s %s %s frames=%ld dropped=%ld interval p50=%.1f p95=%.1f missed=%ld shown p50=%.1f p95=%.1f "
      "nextdrawable p50=%.1f max=%.1f passwait p50=%.2f max=%.2f early=%ld/%ld gpu p50=%.1f",
      label, opt.mtl4 ? "mtl4" : "mtl3", opt.wave ? "wave" : "still", w->frames, w->dropped, pct(&w->interval, 0.5),
      pct(&w->interval, 0.95), w->missed, pct(&w->shown, 0.5), pct(&w->shown, 0.95), pct(&w->next_wait, 0.5),
      pct(&w->next_wait, 1), pct(&w->pass_wait, 0.5), pct(&w->pass_wait, 1), w->early, w->early_known,
      pct(&w->gpu, 0.5)
  );
  if (path)
    printf(" %s", path);
  printf("\n");
}

// one line for the frames collected since the last
static void
print_second(void) {
  char path[160], label[48];
  path_text(path, sizeof(path));
  snprintf(label, sizeof(label), "t=%d moves=%u", ++g_seconds, atomic_exchange(&g_moves, 0));
  print_window(label, &g_win, path);
  window_reset(&g_win);
}

static void
tick(void) {
  collect(false);
  print_second();
}

static void
finish(int code) {
  collect(true);
  if (g_win.frames)
    print_second();
  pointer_stop();
  print_window("summary", &g_sum, NULL);
  printf("summary: %llu frames encoded, %d seconds direct, %d composited, %d unknown\n",
         (unsigned long long)atomic_load(&g_taken), g_direct, g_composited, g_unknown);
  if (g_composited || g_unknown)
    printf("note: the layer wasn't on the direct path for the whole run, so this run can't show #223\n");
  int errors = atomic_load(&g_errors);
  if (errors) {
    printf("FAIL: %d command buffers reported a Metal error\n", errors);
    code = 1;
  } else if (g_sum.frames && g_sum.frames == g_sum.dropped) {
    printf("FAIL: no frame was presented after the first second\n");
    code = 1;
  }
  fflush(stdout);
  exit(code);
}

// --- set-up

@interface PCMetalView : NSView
@end
@implementation PCMetalView
- (CALayer *)makeBackingLayer {
  return [CAMetalLayer layer];
}
- (BOOL)isOpaque {
  return YES;
}
@end

static void
usage(void) {
  printf(
      "usage: present_cursor_test [--present mtl3|mtl4] [--layer-set buffer|queue] [--empty-commit on|off]\n"
      "                           [--present-thread encode|queue] [--pointer still|wave] [--move warp|event]\n"
      "                           [--frames N] [--gpu-ms MS]\n"
      "The three Metal 4 variants take --present mtl4 (the default). See the source's header.\n"
  );
}

static bool
parse(int argc, const char **argv) {
  bool mtl4_only = false;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define IS(name) (!strcmp(a, name) && v && ++i)
    if (IS("--present") && (!strcmp(v, "mtl3") || !strcmp(v, "mtl4")))
      opt.mtl4 = !strcmp(v, "mtl4");
    else if (IS("--layer-set") && (!strcmp(v, "buffer") || !strcmp(v, "queue")))
      opt.set_on_queue = !strcmp(v, "queue"), mtl4_only |= opt.set_on_queue;
    else if (IS("--empty-commit") && (!strcmp(v, "on") || !strcmp(v, "off")))
      opt.empty_commit = !strcmp(v, "on"), mtl4_only |= !opt.empty_commit;
    else if (IS("--present-thread") && (!strcmp(v, "encode") || !strcmp(v, "queue")))
      opt.present_on_queue = !strcmp(v, "queue"), mtl4_only |= opt.present_on_queue;
    else if (IS("--pointer") && (!strcmp(v, "still") || !strcmp(v, "wave")))
      opt.wave = !strcmp(v, "wave");
    else if (IS("--move") && (!strcmp(v, "warp") || !strcmp(v, "event")))
      opt.move_event = !strcmp(v, "event");
    else if (IS("--frames") && (opt.frames = strtol(v, NULL, 10)) > 0)
      ;
    else if (IS("--gpu-ms") && (opt.gpu_ms = strtod(v, NULL)) >= 0)
      ;
    else {
      printf("present_cursor_test: bad argument %s%s%s\n", a, v ? " " : "", v ? v : "");
      return false;
    }
#undef IS
  }
  if (mtl4_only && !opt.mtl4) {
    printf("present_cursor_test: --layer-set, --empty-commit and --present-thread are Metal 4 variants (--present mtl4)\n");
    return false;
  }
  return true;
}

static const char *
arch_name(void) {
#if defined(__x86_64__)
  int translated = 0;
  size_t size = sizeof(translated);
  if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) == 0 && translated == 1)
    return "x86_64 under Rosetta";
  return "x86_64";
#else
  return "arm64";
#endif
}

// the frame pass's GPU time with `iterations`, on the Metal 3 queue; < 0 on an error
static double
cost_ms(uint32_t iterations) {
  Cost *cost = (Cost *)g_costbuf.contents;
  cost->iterations = iterations;
  cost->phase = 0;
  id<MTLCommandBuffer> cb = [g_q3 commandBuffer];
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = g_offscreen;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:g_cost_pso];
  [e setFragmentBuffer:g_costbuf offset:0 atIndex:0];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  if (cb.error) {
    gpu_error("calibration", cb.error);
    return -1;
  }
  return (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
}

static bool
calibrate(void) {
  if (opt.gpu_ms <= 0) {
    g_iterations = 0;
    printf("gpu cost: none (--gpu-ms 0)\n");
    return true;
  }
  uint32_t n = 64;
  cost_ms(n); // the first run pays for the pipeline's first use
  double t = cost_ms(n);
  for (int round = 0; round < 12 && t >= 0; round++) {
    if (fabs(t - opt.gpu_ms) < opt.gpu_ms * 0.05)
      break;
    double scale = t > 0.05 ? opt.gpu_ms / t : 16;
    double next = fmin(fmax((double)n * scale, 1), 1 << 24);
    n = (uint32_t)next;
    t = cost_ms(n);
  }
  if (t < 0)
    return false;
  g_iterations = n;
  printf("gpu cost: %u iterations, %.1f ms a frame pass, measured idle (target %.1f; the GPU's clock may differ in the run)\n",
         n, t, opt.gpu_ms);
  return true;
}

static void
describe_display(CGDirectDisplayID display, NSScreen *screen, uint32_t displays) {
  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
  size_t pw = mode ? CGDisplayModeGetPixelWidth(mode) : 0, ph = mode ? CGDisplayModeGetPixelHeight(mode) : 0;
  double hz = mode ? CGDisplayModeGetRefreshRate(mode) : 0;
  if (mode)
    CGDisplayModeRelease(mode);
  if (hz <= 0)
    hz = (double)screen.maximumFramesPerSecond;
  if (hz > 0)
    g_period = 1.0 / hz;
  uint32_t vendor = CGDisplayVendorNumber(display), model = CGDisplayModelNumber(display);
  const char *kind = CGDisplayIsBuiltin(display) ? "built-in"
                     : vendor                    ? "external"
                                                 : "unknown, no vendor id (maybe a virtual display)";
  printf("display: %u \"%s\", %s, vendor 0x%x model 0x%x, mode %zux%zu pixels at %.2f Hz, %.0fx%.0f points at "
         "scale %.1f, %s, %u active display(s)\n",
         display, screen.localizedName.UTF8String, kind, vendor, model, pw, ph, 1.0 / g_period, screen.frame.size.width,
         screen.frame.size.height, screen.backingScaleFactor,
         CGDisplayIsInMirrorSet(display) ? "mirrored" : "not mirrored", displays);
}

static bool
setup_metal4(void) {
  g_q4 = [g_device newMTL4CommandQueue];
  if (!g_q4)
    return false;
  for (unsigned s = 0; s < kInFlight; s++) {
    g_alloc[s] = [g_device newCommandAllocator];
    g_fcb[s] = [g_device newCommandBuffer];
    g_pcb[s] = [g_device newCommandBuffer];
    g_ecb[s] = [g_device newCommandBuffer];
    if (!g_alloc[s] || !g_fcb[s] || !g_pcb[s] || !g_ecb[s])
      return false;
  }
  NSError *error = nil;
  MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
  ad.maxBufferBindCount = 1;
  ad.maxTextureBindCount = 1;
  g_cost_table = [g_device newArgumentTableWithDescriptor:ad error:&error];
  g_copy_table = [g_device newArgumentTableWithDescriptor:ad error:&error];
  if (!g_cost_table || !g_copy_table) {
    printf("Metal 4: newArgumentTable: %s\n", error.description.UTF8String);
    return false;
  }
  g_raw = [g_device newFence];
  g_war = [g_device newFence];
  // what the frame and the present pass use, on the queue for the whole run (DXMT's device set)
  id<MTLResidencySet> mine = [g_device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
  if (!mine) {
    printf("Metal 4: newResidencySet: %s\n", error.description.UTF8String);
    return false;
  }
  [mine addAllocation:g_offscreen];
  [mine addAllocation:g_costbuf];
  [mine addAllocation:(id<MTLAllocation>)g_cost_pso]; // pipelines need residency on Metal 4 too
  [mine addAllocation:(id<MTLAllocation>)g_copy_pso];
  [mine commit];
  [g_q4 addResidencySet:mine];
  g_layer_set = g_layer.residencySet;
  if (!g_layer_set)
    printf("Metal 4: the layer has no residency set; the drawables may not be resident\n");
  else if (opt.set_on_queue)
    [g_q4 addResidencySet:g_layer_set];
  g_present_queue =
      dispatch_queue_create("present", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  return true;
}

static id<MTLRenderPipelineState>
pipeline(id<MTLLibrary> library, NSString *fragment) {
  MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
  d.vertexFunction = [library newFunctionWithName:@"vs_full"];
  d.fragmentFunction = [library newFunctionWithName:fragment];
  d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  NSError *error = nil;
  id<MTLRenderPipelineState> pso = [g_device newRenderPipelineStateWithDescriptor:d error:&error];
  if (!pso)
    printf("pipeline %s: %s\n", fragment.UTF8String, error.description.UTF8String);
  return pso;
}

static dispatch_source_t
on_signal(int sig) {
  dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sig, 0, dispatch_get_main_queue());
  dispatch_source_set_event_handler(source, ^{
    printf("interrupted\n");
    pointer_stop();
    fflush(stdout);
    exit(130);
  });
  dispatch_resume(source);
  signal(sig, SIG_IGN);
  return source;
}

int
main(int argc, const char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
    usage();
    return 0;
  }
  if (!parse(argc, argv)) {
    usage();
    return 2;
  }
  @autoreleasepool {
    // what the machine has to have, one line each
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session) {
      printf("skip: no GUI session (start it from a terminal in the logged-in desktop, or a LaunchAgent in "
             "gui/<uid>, not from SSH)\n");
      return 77;
    }
    CFBooleanRef console = CFDictionaryGetValue(session, kCGSessionOnConsoleKey);
    CFBooleanRef locked = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
    bool off_console = console && !CFBooleanGetValue(console);
    bool is_locked = locked && CFBooleanGetValue(locked);
    CFRelease(session);
    if (off_console) {
      printf("skip: the GUI session isn't the one on the console\n");
      return 77;
    }
    if (is_locked) {
      printf("skip: the screen is locked\n");
      return 77;
    }
    NSApplication *app = [NSApplication sharedApplication];
    uint32_t displays = 0;
    CGGetActiveDisplayList(0, NULL, &displays);
    NSScreen *screen = NSScreen.screens.firstObject; // the main display, the one with the menu bar
    if (!displays || !screen) {
      printf("skip: no display\n");
      return 77;
    }
    g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) {
      printf("skip: no Metal device\n");
      return 77;
    }
    bool metal4 = [g_device supportsFamily:MTLGPUFamilyMetal4];
    if (opt.mtl4 && !metal4) {
      printf("skip: no Metal 4 on %s (--present mtl4)\n", g_device.name.UTF8String);
      return 77;
    }
    if (opt.wave && opt.move_event && !CGPreflightPostEventAccess()) {
      printf("skip: --move event needs the Accessibility permission for this terminal\n");
      return 77;
    }

    printf("present_cursor_test: present=%s", opt.mtl4 ? "mtl4" : "mtl3");
    if (opt.mtl4)
      printf(" layer-set=%s empty-commit=%s present-thread=%s", opt.set_on_queue ? "queue" : "buffer",
             opt.empty_commit ? "on" : "off", opt.present_on_queue ? "queue" : "encode");
    printf(" pointer=%s", opt.wave ? "wave" : "still");
    if (opt.wave)
      printf(" move=%s (%d a second)", opt.move_event ? "event" : "warp", kPointerHz);
    printf(" frames=%ld gpu-ms=%.1f\n", opt.frames, opt.gpu_ms);
    printf("process: %s\n", arch_name());
    printf("device: %s, Metal 4 %s\n", g_device.name.UTF8String, metal4 ? "yes" : "no");
    CGDirectDisplayID display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    describe_display(display, screen, displays);

    // the window and the layer
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    NSRect frame = screen.frame;
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.level = NSStatusWindowLevel + 1;
    window.opaque = YES;
    app.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
    NSRect bounds = NSMakeRect(0, 0, frame.size.width, frame.size.height);
    PCMetalView *view = [[PCMetalView alloc] initWithFrame:bounds];
    view.wantsLayer = YES;
    window.contentView = view;
    window.contentView.superview.layer.backgroundColor = NULL; // athei/mtld3d #1123
    g_layer = (CAMetalLayer *)view.layer;
    g_layer.device = g_device;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = NO;
    g_layer.displaySyncEnabled = YES;
    g_layer.opaque = YES;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    g_layer.colorspace = srgb;
    CGColorSpaceRelease(srgb);
    g_layer.wantsExtendedDynamicRangeContent = NO;
    g_layer.contentsScale = window.backingScaleFactor;
    g_layer.drawableSize = [view convertSizeToBacking:bounds.size];
    CGSize size = g_layer.drawableSize;
    printf("layer: %.0fx%.0f BGRA8Unorm, framebufferOnly %d, displaySyncEnabled %d, maximumDrawableCount %lu, "
           "opaque %d, sRGB, contentsScale %.1f\n",
           size.width, size.height, g_layer.framebufferOnly, g_layer.displaySyncEnabled,
           (unsigned long)g_layer.maximumDrawableCount, g_layer.opaque, g_layer.contentsScale);
    [window makeKeyAndOrderFront:nil];
    [app activate];

    // the passes
    NSError *error = nil;
    id<MTLLibrary> library = [g_device newLibraryWithSource:@(kShaders) options:nil error:&error];
    if (!library) {
      printf("FAIL: shaders: %s\n", error.description.UTF8String);
      return 1;
    }
    g_cost_pso = pipeline(library, @"fs_cost");
    g_copy_pso = pipeline(library, @"fs_copy");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:(NSUInteger)size.width
                                                                                 height:(NSUInteger)size.height
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    g_offscreen = [g_device newTextureWithDescriptor:td];
    g_costbuf = [g_device newBufferWithLength:kInFlight * kCostStride options:MTLResourceStorageModeShared];
    g_q3 = [g_device newCommandQueue];
    if (!g_cost_pso || !g_copy_pso || !g_offscreen || !g_costbuf || !g_q3) {
      printf("FAIL: Metal objects\n");
      return 1;
    }
    if (!calibrate()) {
      printf("FAIL: the calibration's command buffer failed\n");
      return 1;
    }
    if (opt.mtl4 && !setup_metal4()) {
      printf("FAIL: Metal 4 queue, allocators, command buffers or tables\n");
      return 1;
    }
    for (unsigned s = 0; s < kInFlight; s++)
      g_slot[s] = dispatch_semaphore_create(1);
    g_warm = (uint64_t)lround(1.0 / g_period);

    __attribute__((objc_precise_lifetime)) dispatch_source_t sigint = on_signal(SIGINT);
    __attribute__((objc_precise_lifetime)) dispatch_source_t sigterm = on_signal(SIGTERM);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (opt.wave)
        pointer_start();
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
      pthread_t render;
      pthread_create(&render, &attr, render_main, NULL);
      pthread_attr_destroy(&attr);
      [NSTimer scheduledTimerWithTimeInterval:1.0
                                      repeats:YES
                                        block:^(NSTimer *timer) {
                                          (void)timer;
                                          tick();
                                        }];
    });
    [app run]; // doesn't return: finish() or a signal exits
    (void)sigint;
    (void)sigterm;
  }
  return 0;
}
