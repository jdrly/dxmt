// gw2-test: display
// gw2-build: for arch in arm64 x86_64; do
// gw2-build:   clang -x objective-c -std=gnu17 -fobjc-arc -O1 -Wall -Werror -arch $arch -mmacosx-version-min=26.0 tests/gw2/present_cursor_test.m -framework Cocoa -framework Metal -framework QuartzCore -framework IOSurface -framework CoreGraphics -o ${OUT}_$arch
// gw2-build:   codesign -s - -f ${OUT}_$arch
// gw2-build: done
// gw2-build: rm -rf ${OUT}.app && mkdir -p ${OUT}.app/Contents/MacOS
// gw2-build: cp ${OUT}_arm64 ${OUT}.app/Contents/MacOS/present_cursor_test
// gw2-build: cp tests/gw2/fixtures/present_cursor_test.plist ${OUT}.app/Contents/Info.plist
// gw2-build: codesign -s - -f ${OUT}.app
// gw2-run: R=(tests/gw2/present_cursor.zsh --dir $T --no-sample --pointer still)
// gw2-run: failed=() skipped=0 # each run's own RESULT line, then the failed runs; exit 3 is a variant's result
// gw2-run: run() { "$@" && return; local rc=$?; (( rc == 3 )) && return 0; (( rc == 77 )) && { skipped=1; return 0 }; failed+=("exit $rc: ${(j: :)@[2,-1]}"); return 0 }
// gw2-run: for arch in arm64 x86_64; do
// gw2-run:   for v in "--present mtl3" "--present mtl4" "--present mtl4 --layer-set queue" "--present mtl4 --empty-commit off" "--present mtl4 --present-thread queue" "--present mtl3-on-mtl4" "--present mtl4 --present-at done" "--present mtl3 --encode-thread wine" "--present mtl4 --encode-thread wine" "--present mtl3 --layer wine" "--present mtl4 --layer wine"; do
// gw2-run:     MTL_DEBUG_LAYER=1 run $R --arch $arch ${=v}
// gw2-run:   done
// gw2-run:   for v in "--framebuffer-only on" "--colorspace none" "--transaction on" "--drawable link"; do
// gw2-run:     for p in mtl3 mtl4; do MTL_DEBUG_LAYER=1 run $R --arch $arch --present $p ${=v}; done
// gw2-run:   done
// gw2-run:   for v in "--framebuffer-only on" "--colorspace none"; do MTL_DEBUG_LAYER=1 run $R --arch $arch --present mtl3-on-mtl4 ${=v}; done
// gw2-run:   for p in mtl3 mtl4; do MTL_HUD_ENABLED=1 MTL_DEBUG_LAYER=1 run $R --arch $arch --present $p --hud-lines on; done
// gw2-run:   run $R --arch $arch --present mtl4 --wait-for-drawable off
// gw2-run:   MTL_DEBUG_LAYER=1 $R --arch $arch --present mtl4 --wait-for-drawable off || true # the layer may stop it
// gw2-run: done
// gw2-run: for p in mtl3 mtl4; do MTL_DEBUG_LAYER=1 run $R --bundle --present $p; done
// gw2-run: (( ! ${#failed} )) || { print "failed runs:"; print -l -- $failed; exit 1 }
// gw2-run: (( ! skipped )) || exit 77
//
// gw2 (#225, for #223): on the arm64 engine with Metal 4, moving the cursor makes every frame a
// refresh late (40 fps at 60 Hz with three drawables), and with Metal 3 it doesn't. This is the
// smallest program I could write that might show the same without Wine, FEX, DXMT or the game: one
// borderless full-screen window, one CAMetalLayer set up as my presenter sets it up, a frame with a
// GPU cost I can dial, and the present done the Metal 3 way or the Metal 4 way, while a thread moves
// the pointer. It changes nothing in the driver.
//
// It isn't part of `tests/gw2/run.zsh all`: it needs a logged-in GUI session, a display, the pointer,
// and for the Metal 4 ways a Metal 4 device. `tests/gw2/run.zsh display` builds it and runs the
// acceptance runs above (both ways and each variant, 600 frames, MTL_DEBUG_LAYER=1, arm64 and x86_64,
// pointer still), and prints one RESULT line a run. GitHub's runner only builds it
// (`run.zsh display --build-only`). One run with a window server sample beside it is one command,
// which builds what it needs, asks for sudo once and ends with one RESULT line (tests/gw2/present_cursor.zsh):
//
//   tests/gw2/present_cursor.zsh --present mtl4 --pointer wave
//   MTL_HUD_ENABLED=1 tests/gw2/present_cursor.zsh --present mtl4 --pointer wave
//   tests/gw2/present_cursor.zsh --bundle --present mtl4 --pointer wave     # from the Game Mode bundle
//
// Run it from a terminal inside the GUI session, with the game closed and nothing over the window.
//
// What it sets up, from my fork (src/dxmt/dxmt_presenter.cpp's constructor, src/winemetal/unix/
// winemetal_unix.c's _MetalLayer_setProps and _MetalLayer_setColorSpace): BGRA8Unorm, framebufferOnly
// off, display sync on, opaque, colorspace sRGB without EDR, maximumDrawableCount left at its 3 (the
// defaults; --framebuffer-only, --colorspace and --transaction change one each, below). The
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
//    --present mtl3-on-mtl4 (#227's DXMT_MTL4_PRESENT=mtl3): the frame pass on the Metal 4 queue, the
//      present as --present mtl3 does it in a buffer of a Metal 3 queue on the same device. The Metal 4
//      queue signals a shared event after the frame pass and the Metal 3 buffer waits for it; the buffer
//      signals a second one and the Metal 4 queue's later commits wait for that. No waitForDrawable, no
//      signalDrawable, no layer set.
//    Metal 4 variants, one flag each; they combine:
//      --layer-set queue         the layer's set on the queue for the whole run, as before #184 (mtl4)
//      --empty-commit off        no empty command buffer after the present (DXMT_MTL4_TIMESTAMP_REUSE=1;
//                                mtl4 and mtl3-on-mtl4)
//      --present-thread queue    presentAtTime: from a serial dispatch queue, not the encoding thread (mtl4)
//      --present-at done         (#230, mtl4) signalDrawable where it was, but presentAtTime: from the present
//                                pass's commit feedback handler, once its GPU work has completed. The target
//                                is the one the encoding thread computed after signalDrawable, one refresh
//                                ahead; only the moment of the call moves, so the handler may call it after
//                                the target. If the encoding thread hasn't reached signalDrawable when the
//                                handler runs, the encoding thread presents right after it, so no frame is
//                                presented before its signal. A pass with a Metal error isn't presented and
//                                counts as dropped. The present then comes from a Metal thread too: read it
//                                against --present-thread queue, which moves the thread and not the time
//                                (with both, the handler hands the present to that queue)
//      --wait-for-drawable off   (#230, mtl4) no waitForDrawable; everything else stays. This breaks Apple's
//                                rule for the call. It is a probe: the debug layer may stop it, and the pass
//                                may draw into a surface the display still shows (a moving image tears)
//    What a variant's window server sample means (#230). Read the frame-info thread
//    (com.apple.coreanimation.frameinfo.<display>) under frame_info_callback, and of it kern_SwapWait,
//    next to interval p95, missed, passwait and early. Quiet is Metal 3's level on my Mac, 2 or 3
//    samples in 600; busy is Metal 4's, 11 or more:
//      --present-at done, quiet and flat while waving: the window server waits per frame only for a frame
//        presented before its GPU work ran; the early present is the cause, and the fix presents later
//      --present-at done quiet, and --present-thread queue quiet too: the calling thread did it, not the time
//      --present-at done, busy: the wait belongs to the Metal 4 sequence whenever the present arrives
//      --wait-for-drawable off, quiet: the frame-info work is the display's side of waitForDrawable, the
//        window server waiting for a swap so it can say the display is done with a drawable
//      --wait-for-drawable off, busy: the wait call isn't what asks for it
//      both, busy in every combination: it belongs to a drawable that presents itself after signalDrawable;
//        only Metal 3's way avoids it, which is what #227's mtl3 does
//    With --wait-for-drawable off also compare passwait and early with the run with the wait on: unchanged
//    means the wait never held the present pass.
//    What the game's process has and this one hasn't (#230, after the M1 run), one flag each, every way:
//      --hud-lines on            five label lines a frame into the Metal HUD through MTLHUDService
//                                (_CADeveloperHUDProperties before macOS 27), from the encoding thread right
//                                after the present, under the heading label, as d3d11_swapchain.cpp's
//                                UpdateStatistics and winemetal_unix.c write them. Private. Without the class
//                                or its instance (the HUD off) it says so in one line and runs without them.
//                                Only the HUD itself (MTL_HUD_ENABLED=1, no flag) is in every game session;
//                                a release build of my fork writes the heading and not the five lines
//      --encode-thread wine      the encoding thread without a QoS class, with the Mach policies wineserver
//                                sets for THREAD_PRIORITY_TIME_CRITICAL (server/thread.c's
//                                apply_thread_priority at effective priority 15: latency and throughput QoS
//                                tiers unspecified, timeshare off, precedence 32). The default is a thread
//                                with QOS_CLASS_USER_INTERACTIVE. On Metal 4 this thread calls the present
//      --layer wine              the layer as Mach9's Mac driver makes it (dlls/winemac.drv/cocoa_window.m,
//                                WineMetalView): framebufferOnly on at creation (my presenter turns it off),
//                                nearest magnification filter, black background, redraw policy never, the
//                                Metal view as the lowest subview of a layer-backed content view whose own
//                                layer holds an image (black, the window's size)
//    How the drawable is handed back (#245, for #223). A Metal 3 present's surface is in use by the window
//    server when its drawable comes back (inuse on every frame), a Metal 4 present's never is (inuse 0). Each
//    of these could change that; none has evidence behind it. Each takes mtl3 and mtl4 and combines with the
//    others; mtl3-on-mtl4 takes the first two. The start line names each one that is on:
//      --framebuffer-only on     the layer's framebufferOnly on (my presenter turns it off). The present pass
//                                only renders into the drawable, so nothing else changes
//      --colorspace none         the layer's colorspace left nil, and wantsExtendedDynamicRangeContent not set
//      --transaction on          the layer's presentsWithTransaction on, and the present as Apple's rule for it
//                                says: mtl3 commits the buffer, waits until it is scheduled, then calls the
//                                drawable's present. A Metal 4 queue has no waitUntilScheduled, and Apple's docs
//                                say nothing of Metal 4 with this layer, so mtl4 commits the pass, calls
//                                signalDrawable, signals a shared event on the queue and waits for it (the pass
//                                and the signal have run, later than scheduled), then presents. Both from the
//                                encoding thread, inside an explicit CATransaction (that thread has no run loop
//                                to commit an implicit one), with no time. So not with --present-at done or
//                                --present-thread queue. If mtl4 presents no frame this way, that is the
//                                variant's result: a result: line, exit 3
//      --drawable link           the drawable from a CAMetalDisplayLink on the layer, not from nextDrawable.
//                                The link runs on the encoding thread's run loop at the display's refresh rate,
//                                with --link-latency 1|2 (default 2; Apple takes no other) as its
//                                preferredFrameLatency. Each callback encodes one frame: it waits for frame k - 3
//                                as the loop does, then the frame pass, the present pass into the update's
//                                drawable, and the present for the chosen way, the plain one: the link times it,
//                                and Apple says a present for a specific time asserts with a display link. The
//                                callback that encodes the last frame invalidates the link and stops the run
//                                loop, and the thread is joined before the summary. So the frame starts when the
//                                layer hands the drawable out, the reverse of DXMT's order (the frame first, then
//                                nextDrawable). nextdrawable prints n/a, shown counts from the callback's start,
//                                and the surface's use is read there
//    What a result means. Read each variant against the baseline of the same present way, still and waving,
//    each with a window server sample: refreshes 1= 2=, passwait, early, inuse, and the frame-info thread's
//    frame_info_callback and swap_wait counts.
//      with the variant, --present mtl4 waving           meaning
//      flat where the baseline saws, inuse every frame   the variant moves a Metal 4 present onto the use-count
//                                                        hand-back: a property my presenter can set, and an
//                                                        issue of its own for the game
//      flat, inuse still 0                               it changes the frame's path another way; read the
//                                                        sample for the copy (copy_surface) or the compositor
//                                                        before calling it a fix
//      saws as the baseline                              that property is out
//      flat with every variant and with none             the test doesn't reproduce #223 on this machine, and
//                                                        nothing here says anything
//    For --drawable link also read shown against the baseline's: a link that is flat at one more refresh of
//    latency is a trade, not a fix.
//    Game Mode: `run.zsh display` also puts the arm64 binary into ${OUT}.app with LSSupportsGameMode,
//    GCSupportsGameMode and a games category (tests/gw2/fixtures/present_cursor_test.plist), as the game's
//    process runs from a bundle with those keys. Started from the bundle, the start line says bundle=<id>
//    and nothing else differs. Whether macOS turned Game Mode on shows only in its menu bar item.
// --pointer wave moves the pointer in a circle (a quarter of the screen's height) 80 times a second,
// one turn a second, with CGWarpMouseCursorPosition, or with posted mouse-moved events with
// --move event (needs the Accessibility permission for the terminal). Whether a warp makes the window
// server update its cursor the way a mouse does I haven't checked; the start line says which way it
// used. The pointer goes back where it was at the end, also after Ctrl-C or SIGTERM.
//
// Output. At start: the start line with every variant in effect, the process's architecture (Rosetta
// or not), the device, the display (size, refresh rate, built-in or external, vendor and model), the
// layer, the calibrated cost, and a line for each of --hud-lines, --encode-thread wine, --layer wine,
// --transaction on and --drawable link when on. Then one line a second, for the frames whose GPU work
// and presented handler have come back:
//   frames, dropped    frames taken; dropped: not presented (presentedTime 0, or no drawable)
//   interval p50/p95   presentedTime minus the last presented frame's, ms. 16.7 is flat at 60 Hz
//   missed             refreshes skipped between presented frames (an interval of two refreshes is 1)
//   shown p50/p95      presentedTime minus the time nextDrawable returned (the callback's start with
//                      --drawable link), ms
//   nextdrawable       how long nextDrawable blocked, ms; n/a with --drawable link
//   passwait           the present pass's GPU start minus its commit, ms: how long Metal held the pass
//   early n/m          of the m frames whose drawable's surface had been on screen before, n started
//                      their present pass before the frame after that surface's last one was presented,
//                      so while that surface may still have been on screen (by the layer's own clock)
//   inuse n/m          (#245) of the m drawables taken (with a surface), n whose surface IOSurfaceIsInUse
//                      reported in use right after the take, before anything was encoded into it
//   gpu p50            the frame pass's GPU time, ms
//   moves              pointer moves in that second
//   path               the layer's private displayCompositingInternalStatus, read on the main thread if
//                      the layer responds to it: 0 is direct (athei found it 0 whenever the Metal HUD
//                      said Direct), anything else is composited. A composited line says so.
// At the end, one summary line over the frames after the first second, then (#237) an intervals line
// over the same intervals: n, p50, p95 and max in ms, and how many were 1, 2, 3, and 4 or more refreshes
// (the interval over the display's refresh period, rounded), with that period. If any second was
// composited or unknown, a line says this run can't show #223.
//
// What it can't tell: whether the cursor is a hardware plane (no public read-out; the window server's
// sample shows cursor_callback and set_cursor_surface), and a flip from a copy on the direct path (the
// status is 0 for both, by athei's reading; the window server's sample shows copy_surface).
//
// Exit: 0 when every frame was encoded and no command buffer reported an error; 1 on a Metal error, if no
// frame was presented, or if the display link stopped before the last frame; 2 on a bad argument or a
// flag the present way doesn't take; 3 when --present mtl4 --transaction on presented no frame (the
// variant's result, not a failure); 77 (skipped) without a GUI session, without a
// display, without a Metal device, without Metal 4 for either Metal 4 way, without the permission for
// --move event, or when the layer gives no drawable three times in a row (the window isn't on a
// display); 130 after Ctrl-C.

#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <mach/mach.h>
#include <mach/task_policy.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
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
  bool mtl4, mtl3_present, set_on_queue, empty_commit, present_on_queue, wave, move_event;
  bool present_at_done, wait_for_drawable, hud_lines, encode_wine, layer_wine; // #230
  bool framebuffer_only, colorspace_none, transaction, link;                   // #245
  int link_latency;
  long frames;
  double gpu_ms;
} opt = {.mtl4 = true, .empty_commit = true, .wait_for_drawable = true, .link_latency = 2, .frames = 600, .gpu_ms = 13};

// --- what the threads share

typedef struct {
  uint64_t frame;
  double take, next_wait, pass_commit, pass_start, gpu_cost, presented;
  uint32_t surface;
  bool inuse; // IOSurfaceIsInUse of the drawable's surface when taken
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
static id<MTLSharedEvent> g_frame_event, g_shown_event; // --present mtl3-on-mtl4
static id<MTLSharedEvent> g_drawn_event; // --transaction on: the queue has run a frame's pass and signalDrawable
static dispatch_queue_t g_present_queue;
// --drawable link: the update the callback is encoding, on the link's thread
static id<CAMetalDrawable> g_link_drawable;
static double g_link_time; // the callback's start
static bool g_link_inuse;

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

// --- what the game's process has (#230)

// --hud-lines on: the Metal HUD's label lines as my fork writes them (dxmt_hud_state.cpp, and
// winemetal_unix.c's _DeveloperHUDProperties_*), all through objc_msgSend on private classes
static id g_hud;
static bool g_hud_metric; // MTLHUDService's insertMetric:, from macOS 27
static NSString *g_hud_labels[6]; // the heading, then the five lines

static void
hud_add(NSString *label, NSString *after) {
  if (g_hud_metric)
    ((bool (*)(id, SEL, id, id, id, id, uint32_t, uint32_t, uint32_t, uint64_t))objc_msgSend)(
        g_hud, sel_registerName("insertMetric:after:name:unit:nameColor:valueColor:visualType:options:"), label,
        after, @"", @"", UINT32_MAX, UINT32_MAX, 1u, 0u
    );
  else
    ((bool (*)(id, SEL, id, id))objc_msgSend)(g_hud, sel_registerName("addLabel:after:"), label, after);
}

static void
hud_update(NSString *label, NSString *value) {
  ((void (*)(id, SEL, id, id))objc_msgSend)(g_hud, sel_registerName("updateLabel:value:"), label, value);
}

// at start: the class, its instance and the heading (HUDState::initialize); without them, one line
static void
hud_setup(void) {
  g_hud_metric = NSProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27;
  const char *name = g_hud_metric ? "MTLHUDService" : "_CADeveloperHUDProperties";
  Class cls = objc_lookUpClass(name);
  SEL instance = sel_registerName("instance");
  if (cls && [cls respondsToSelector:instance])
    g_hud = ((id (*)(id, SEL))objc_msgSend)(cls, instance);
  if (!g_hud) {
    printf("hud-lines: no %s%s in this process (MTL_HUD_ENABLED=%s); running without the lines\n", name,
           cls ? " instance" : "", getenv("MTL_HUD_ENABLED") ?: "unset");
    opt.hud_lines = false;
    return;
  }
  g_hud_labels[0] = @"com.github.3shain.dxmt-version";
  for (int i = 1; i < 6; i++)
    g_hud_labels[i] = [NSString stringWithFormat:@"com.github.3shain.dxmt-line%d", i];
  hud_add(g_hud_labels[0], @"com.apple.hud-graph.default");
  hud_update(g_hud_labels[0], @"present_cursor_test");
  printf("hud-lines: %s, five lines a frame from the encoding thread\n", name);
}

// each frame, right after its present (UpdateStatistics): five lines of DXMT's lengths, each label
// added after the one before the first time
static void
hud_frame(uint64_t k) {
  static bool added;
  if (!added) {
    for (int i = 1; i < 6; i++)
      hud_add(g_hud_labels[i], g_hud_labels[i - 1]);
    added = true;
  }
  hud_update(g_hud_labels[1], @"---------------------------");
  hud_update(g_hud_labels[2], [NSString stringWithFormat:@"Commit: %2u -%4.1f -%4.1f", opt.mtl4 ? 3u : 2u,
                                                         g_period * 1e3, g_period * 1e3]);
  hud_update(g_hud_labels[3], [NSString stringWithFormat:@"Sync:   %2d %4.1f  %2d %4.1f %2d", 0, 0.0, 0, 0.0,
                                                         kInFlight]);
  hud_update(g_hud_labels[4], [NSString stringWithFormat:@"Encode: %4.1f+%4.1f+%4.1f=%4.1f", 0.1, 0.1, 0.0, 0.2]);
  hud_update(g_hud_labels[5], [NSString stringWithFormat:@"Render:%3llu+%-3d Clear:%3d+%-2d",
                                                         (unsigned long long)(k % 1000), 0, 0, 0]);
}

// --encode-thread wine: what wineserver gives a THREAD_PRIORITY_TIME_CRITICAL thread on macOS
// (server/thread.c's apply_thread_priority, effective priority 15 in a normal priority class: the
// application band, not the realtime one). The thread is created without a QoS class.
static void
wine_thread_policies(void) {
  mach_port_t port = pthread_mach_thread_np(pthread_self());
  thread_latency_qos_policy_data_t latency = {LATENCY_QOS_TIER_UNSPECIFIED};
  thread_throughput_qos_policy_data_t throughput = {THROUGHPUT_QOS_TIER_UNSPECIFIED};
  thread_extended_policy_data_t extended = {.timeshare = 0};          // effective priority > 14
  thread_precedence_policy_data_t precedence = {.importance = 32};    // -31 + (15 - 1) * 63 / 14
  kern_return_t kr[4] = {
      thread_policy_set(port, THREAD_LATENCY_QOS_POLICY, (thread_policy_t)&latency, THREAD_LATENCY_QOS_POLICY_COUNT),
      thread_policy_set(port, THREAD_THROUGHPUT_QOS_POLICY, (thread_policy_t)&throughput,
                        THREAD_THROUGHPUT_QOS_POLICY_COUNT),
      thread_policy_set(port, THREAD_EXTENDED_POLICY, (thread_policy_t)&extended, THREAD_EXTENDED_POLICY_COUNT),
      thread_policy_set(port, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&precedence, THREAD_PRECEDENCE_POLICY_COUNT),
  };
  printf("encode-thread: wine: latency and throughput QoS tiers unspecified, timeshare 0, precedence 32 "
         "(kern_return %d %d %d %d), QoS class 0x%x\n",
         kr[0], kr[1], kr[2], kr[3], (unsigned)qos_class_self());
}

// --- the frame

// nextDrawable, timed, or with --drawable link the update's drawable, taken at the callback's start; the
// surface's use is read here, before anything is encoded into the drawable; the presented handler goes on
// before any present
static id<CAMetalDrawable>
take(uint64_t k) {
  double t0 = opt.link ? g_link_time : CACurrentMediaTime();
  id<CAMetalDrawable> d = opt.link ? g_link_drawable : [g_layer nextDrawable];
  double t1 = opt.link ? g_link_time : CACurrentMediaTime();
  IOSurfaceRef surface = d ? d.texture.iosurface : NULL;
  bool inuse = opt.link ? g_link_inuse : (surface && IOSurfaceIsInUse(surface));
  os_unfair_lock_lock(&g_lock);
  frame_at(k)->take = t1;
  frame_at(k)->next_wait = t1 - t0;
  if (!d) {
    frame_at(k)->no_drawable = true;
    frame_at(k)->shown_done = true;
  } else {
    frame_at(k)->surface = surface ? IOSurfaceGetID(surface) : 0;
    frame_at(k)->inuse = inuse;
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

// the drawable's present at `at`; with --drawable link the plain one: the link times the present, and Apple
// says a present for a specific time asserts there
static void
present_at(id<CAMetalDrawable> d, double at) {
  if (opt.link)
    [d present];
  else
    [d presentAtTime:at];
}

// --transaction on: the drawable's plain present inside an explicit transaction, on the encoding thread,
// which has no run loop to commit an implicit one
static void
present_in_transaction(id<CAMetalDrawable> d) {
  [CATransaction begin];
  [d present];
  [CATransaction commit];
}

// the present pass into a Metal 3 command buffer, and the command buffer's present
// (dxmt_presenter.cpp's encodeCommands, dxmt_context.cpp's Metal 3 branch); with --transaction on the
// caller presents after the commit
static void
encode_present3(id<MTLCommandBuffer> pcb, id<CAMetalDrawable> d) {
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = d.texture;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> e = [pcb renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:g_copy_pso];
  [e setFragmentTexture:g_offscreen atIndex:0];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e endEncoding];
  if (opt.link && !opt.transaction)
    [pcb presentDrawable:d]; // the link times it
  else if (!opt.transaction)
    [pcb presentDrawable:d atTime:CACurrentMediaTime() + g_period];
}

static void
commit_present3(uint64_t k, id<MTLCommandBuffer> pcb) {
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
  encode_present3(pcb, d);
  commit_present3(k, pcb);
  if (opt.transaction) { // Apple's order for presentsWithTransaction
    [pcb waitUntilScheduled];
    present_in_transaction(d);
  }
  if (opt.hud_lines)
    hud_frame(k);
}

enum { kFramePass, kPresentPass, kEmpty };
static const char *kCommitNames[] = {"frame pass", "present pass", "empty command buffer"};

// `then`, if any, runs in the feedback handler once the GPU work has completed, before the frame lets go
static void
commit4(uint64_t k, id<MTL4CommandBuffer> cb, int kind, void (^then)(bool failed)) {
  frame_ref(k);
  MTL4CommitOptions *options = [MTL4CommitOptions new]; // one per commit, as mtl4.c does
  [options addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
    if (fb.error)
      gpu_error(kCommitNames[kind], fb.error);
    if (kind == kFramePass)
      SET(k, gpu_cost, fb.GPUEndTime - fb.GPUStartTime);
    else if (kind == kPresentPass)
      SET(k, pass_start, fb.GPUStartTime);
    if (then)
      then(fb.error != nil);
    frame_unref(k);
  }];
  __unsafe_unretained id<MTL4CommandBuffer> list[1] = {cb};
  [g_q4 commit:list count:1 options:options];
}

// --present-at done: the drawable's present once the pass has completed and signalDrawable was called,
// by whichever of the feedback handler and the encoding thread comes second; the block holds the drawable
typedef struct {
  _Atomic int arrived; // the pass's completion and signalDrawable, each adds one
  double at;           // the target, from the encoding thread before it arrives
  bool failed;         // the pass reported an error, from the handler before it arrives
} PresentGate;

static void
gate_arrive(uint64_t k, id<CAMetalDrawable> d, PresentGate *g) {
  if (atomic_fetch_add(&g->arrived, 1) != 1)
    return; // the other one presents
  if (g->failed) {
    SET(k, shown_done, true); // never presented: presented stays 0, so the frame counts as dropped
  } else if (opt.present_on_queue) {
    double at = g->at;
    dispatch_async(g_present_queue, ^{
      present_at(d, at);
    });
  } else {
    present_at(d, g->at);
  }
  free(g);
}

// the present the Metal 4 way (dxmt_context.cpp's Metal 4 branch, encodeCommandsMetal4, mtl4.c)
static void
present_mtl4(uint64_t k, unsigned s) {
  id<CAMetalDrawable> d = take(k);
  if (!d)
    return;
  if (opt.wait_for_drawable)
    [g_q4 waitForDrawable:d];
  id<MTL4CommandBuffer> pcb = g_pcb[s];
  [pcb beginCommandBufferWithAllocator:g_alloc[s]];
  if (!opt.set_on_queue && g_layer_set)
    [pcb useResidencySet:g_layer_set];
  MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
  rp.colorAttachments[0].texture = d.texture;
  rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTL4RenderCommandEncoder> e = [pcb renderCommandEncoderWithDescriptor:rp];
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
  if (opt.present_at_done) {
    PresentGate *g = calloc(1, sizeof(*g));
    commit4(k, pcb, kPresentPass, ^(bool failed) {
      g->failed = failed;
      gate_arrive(k, d, g);
    });
    [g_q4 signalDrawable:d];
    g->at = CACurrentMediaTime() + g_period;
    gate_arrive(k, d, g);
    return;
  }
  commit4(k, pcb, kPresentPass, nil);
  [g_q4 signalDrawable:d];
  if (opt.transaction) {
    // Apple's rule presents after the pass is scheduled; a Metal 4 queue has no waitUntilScheduled, so wait
    // until it has run the pass and signalDrawable (later than scheduled), then present
    [g_q4 signalEvent:g_drawn_event value:k + 1];
    static int late;
    if (![g_drawn_event waitUntilSignaledValue:k + 1 timeoutMS:1000] && late++ < 5)
      printf("transaction: frame %llu: the queue hadn't run the pass and signalDrawable in 1 s; presenting anyway\n",
             (unsigned long long)k);
    present_in_transaction(d);
  } else if (opt.present_on_queue) {
    dispatch_async(g_present_queue, ^{
      present_at(d, CACurrentMediaTime() + g_period);
    });
  } else {
    present_at(d, CACurrentMediaTime() + g_period);
  }
}

// --present mtl3-on-mtl4, #227's DXMT_MTL4_PRESENT=mtl3 (Mtl4Encoding::beginMetal3Present and
// commitMetal3Present): the frame stays on the Metal 4 queue; the pass and the present are a Metal 3
// command buffer's. The Metal 4 queue signals `frame` after the frame pass and the Metal 3 buffer waits
// for it; the buffer signals `shown` and the Metal 4 queue's later commits wait for it. No
// waitForDrawable, no signalDrawable, no layer set: Metal 3 does those.
static void
present_mtl3_on_mtl4(uint64_t k) {
  uint64_t value = k + 1;
  [g_q4 signalEvent:g_frame_event value:value];
  id<MTLCommandBuffer> pcb = [g_q3 commandBuffer];
  [pcb encodeWaitForEvent:g_frame_event value:value];
  id<CAMetalDrawable> d = take(k);
  if (d)
    encode_present3(pcb, d);
  [pcb encodeSignalEvent:g_shown_event value:value]; // also without a drawable: the queue waits for it
  // as Mtl4Queue::waitMetal3: a failed buffer may never signal, so the CPU does, or the queue stops
  [pcb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
    if (cb.status == MTLCommandBufferStatusError && g_shown_event.signaledValue < value)
      g_shown_event.signaledValue = value;
  }];
  commit_present3(k, pcb);
  [g_q4 waitForEvent:g_shown_event value:value];
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
  commit4(k, fcb, kFramePass, nil);
  if (opt.mtl3_present)
    present_mtl3_on_mtl4(k);
  else
    present_mtl4(k, s);
  if (opt.hud_lines) // before the chunk's end, where DXMT's empty commit goes
    hud_frame(k);
  if (opt.empty_commit) {
    [g_ecb[s] beginCommandBufferWithAllocator:g_alloc[s]];
    [g_ecb[s] endCommandBuffer];
    commit4(k, g_ecb[s], kEmpty, nil);
  }
}

static void finish(int code);

// frame k, once frame k - 3 is done on the GPU; false when the run stops (no drawable three times in a row)
static bool
encode_frame(uint64_t k) {
  static int no_drawable_run;
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
    return false;
  }
  return true;
}

// --drawable link (#245): each callback encodes one frame into the update's drawable, on the encoding
// thread's run loop; the one that encodes the last frame invalidates the link and stops the run loop
@interface PCLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@end
@implementation PCLinkDelegate {
  uint64_t _next;
}
- (void)metalDisplayLink:(CAMetalDisplayLink *)displayLink needsUpdate:(CAMetalDisplayLinkUpdate *)update {
  if (_next >= (uint64_t)opt.frames)
    return;
  @autoreleasepool {
    g_link_time = CACurrentMediaTime();
    g_link_drawable = update.drawable;
    IOSurfaceRef surface = g_link_drawable.texture.iosurface;
    g_link_inuse = surface && IOSurfaceIsInUse(surface);
    encode_frame(_next++);
    g_link_drawable = nil;
  }
  if (_next == (uint64_t)opt.frames) {
    [displayLink invalidate];
    CFRunLoopStop(CFRunLoopGetCurrent());
  }
}
@end

static CAMetalDisplayLink *g_link;
static PCLinkDelegate *g_link_delegate; // the link holds it weakly

static void
link_run(void) {
  @autoreleasepool {
    g_link_delegate = [PCLinkDelegate new];
    g_link = [[CAMetalDisplayLink alloc] initWithMetalLayer:g_layer];
    g_link.delegate = g_link_delegate;
    g_link.preferredFrameLatency = (float)opt.link_latency;
    float hz = (float)(1.0 / g_period);
    g_link.preferredFrameRateRange = CAFrameRateRangeMake(hz, hz, hz);
    [g_link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSDefaultRunLoopMode];
  }
  CFRunLoopRun(); // until the last frame's callback stops it
}

static pthread_t g_render; // the encoding thread, joined before the report

static void *
render_main(void *arg) {
  (void)arg;
  if (opt.encode_wine)
    wine_thread_policies();
  if (opt.link) {
    link_run();
  } else {
    for (uint64_t k = 0; k < (uint64_t)opt.frames; k++) {
      @autoreleasepool {
        if (!encode_frame(k))
          return NULL;
      }
    }
  }
  for (unsigned s = 0; s < kInFlight; s++)
    dispatch_semaphore_wait(g_slot[s], DISPATCH_TIME_FOREVER); // everything done on the GPU
  usleep(300000); // the last frames' presented handlers
  int code = 0;
  if (atomic_load(&g_taken) < (uint64_t)opt.frames) {
    printf("FAIL: the display link's run loop ended after %llu of %ld frames\n",
           (unsigned long long)atomic_load(&g_taken), opt.frames);
    code = 1;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    pthread_join(g_render, NULL);
    finish(code);
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
  long inuse, inuse_known; // drawables whose surface was in use when taken, of those with a surface
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
static long g_presented_all; // over the whole run, the first second too

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
  w->frames = w->dropped = w->missed = w->early = w->early_known = w->inuse = w->inuse_known = 0;
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
  if (f->surface && !f->no_drawable) {
    w->inuse_known++;
    w->inuse += f->inuse;
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
      g_presented_all++;
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

static const char *
present_name(void) {
  return opt.mtl3_present ? "mtl3-on-mtl4" : opt.mtl4 ? "mtl4" : "mtl3";
}

static void
print_window(const char *label, Window *w, const char *path) {
  printf("%s %s %s frames=%ld dropped=%ld interval p50=%.1f p95=%.1f missed=%ld shown p50=%.1f p95=%.1f ", label,
         present_name(), opt.wave ? "wave" : "still", w->frames, w->dropped, pct(&w->interval, 0.5),
         pct(&w->interval, 0.95), w->missed, pct(&w->shown, 0.5), pct(&w->shown, 0.95));
  if (opt.link) // the link hands the drawable out; nothing waits for it
    printf("nextdrawable n/a");
  else
    printf("nextdrawable p50=%.1f max=%.1f", pct(&w->next_wait, 0.5), pct(&w->next_wait, 1));
  printf(" passwait p50=%.2f max=%.2f early=%ld/%ld inuse=%ld/%ld gpu p50=%.1f", pct(&w->pass_wait, 0.5),
         pct(&w->pass_wait, 1), w->early, w->early_known, w->inuse, w->inuse_known, pct(&w->gpu, 0.5));
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
  if (opt.link) { // a link that stops calling back would leave the run waiting for ever
    static uint64_t last;
    static int quiet;
    uint64_t taken = atomic_load(&g_taken);
    quiet = taken == last ? quiet + 1 : 0;
    last = taken;
    if (quiet == 3 && taken < (uint64_t)opt.frames) {
      printf("FAIL: the display link encoded no frame in three seconds, after %llu of %ld\n",
             (unsigned long long)taken, opt.frames);
      pointer_stop();
      exit(1);
    }
  }
}

static void
finish(int code) {
  collect(true);
  if (g_win.frames)
    print_second();
  pointer_stop();
  print_window("summary", &g_sum, NULL);
  // the summary's intervals by whole refreshes of the display's own rate; pct() sorts, so after the summary
  long refreshes[5] = {0};
  for (size_t i = 0; i < g_sum.interval.n; i++) {
    long r = lround(g_sum.interval.v[i] / g_period);
    refreshes[r < 1 ? 1 : r > 4 ? 4 : r]++;
  }
  printf("intervals: n=%zu p50=%.1f p95=%.1f max=%.1f refreshes 1=%ld 2=%ld 3=%ld 4+=%ld (period %.2f ms)\n",
         g_sum.interval.n, pct(&g_sum.interval, 0.5), pct(&g_sum.interval, 0.95), pct(&g_sum.interval, 1),
         refreshes[1], refreshes[2], refreshes[3], refreshes[4], g_period * 1e3);
  printf("summary: %llu frames encoded, %d seconds direct, %d composited, %d unknown\n",
         (unsigned long long)atomic_load(&g_taken), g_direct, g_composited, g_unknown);
  if (g_composited || g_unknown)
    printf("note: the layer wasn't on the direct path for the whole run, so this run can't show #223\n");
  if (opt.wave)
    printf("note: flat frames count against #223 only if the window server's sample shows the cursor as its "
           "own plane (cursor_callback, set_cursor_surface)\n");
  int errors = atomic_load(&g_errors);
  if (errors) {
    printf("FAIL: %d command buffers reported a Metal error\n", errors);
    code = 1;
  } else if (!g_presented_all && opt.mtl4 && opt.transaction) {
    // the variant's result, not the test's failure (#245): the present followed Apple's rule as far as Metal 4 can
    printf("result: no frame was presented: Metal 4 doesn't present a drawable of a layer with "
           "presentsWithTransaction this way\n");
    code = 3;
  } else if (!g_presented_all) {
    printf("FAIL: no frame was presented\n");
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
  CAMetalLayer *layer = [CAMetalLayer layer];
  if (opt.layer_wine) { // WineMetalView's makeBackingLayer
    layer.device = g_device;
    layer.framebufferOnly = YES;
    layer.magnificationFilter = kCAFilterNearest;
    layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
  }
  return layer;
}
- (BOOL)isOpaque {
  return YES;
}
@end

// --layer wine: WineContentView, whose own layer holds the window surface's image (updateLayer)
static CGImageRef g_window_image;

@interface PCImageView : NSView
@end
@implementation PCImageView
- (BOOL)isFlipped {
  return YES;
}
- (BOOL)wantsUpdateLayer {
  return YES;
}
- (void)updateLayer {
  self.layer.contents = (__bridge id)g_window_image;
}
@end

// black, opaque, `w` x `h` pixels
static CGImageRef
black_image(size_t w, size_t h) {
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, srgb, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
  CGColorSpaceRelease(srgb);
  if (!c)
    return NULL;
  CGContextSetRGBFillColor(c, 0, 0, 0, 1);
  CGContextFillRect(c, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h));
  CGImageRef image = CGBitmapContextCreateImage(c);
  CGContextRelease(c);
  return image;
}

static void
usage(void) {
  printf(
      "usage: present_cursor_test [--present mtl3|mtl4|mtl3-on-mtl4] [--layer-set buffer|queue] [--empty-commit on|off]\n"
      "                           [--present-thread encode|queue] [--present-at signal|done]\n"
      "                           [--wait-for-drawable on|off] [--hud-lines off|on] [--encode-thread interactive|wine]\n"
      "                           [--layer plain|wine] [--framebuffer-only off|on] [--colorspace srgb|none]\n"
      "                           [--transaction off|on] [--drawable next|link] [--link-latency 1|2]\n"
      "                           [--pointer still|wave] [--move warp|event] [--frames N] [--gpu-ms MS]\n"
      "--layer-set, --present-thread, --present-at and --wait-for-drawable take --present mtl4 (the default),\n"
      "--empty-commit either Metal 4 way, --transaction and --drawable mtl3 and mtl4, --link-latency\n"
      "--drawable link. See the source's header.\n"
  );
}

static bool
parse(int argc, const char **argv) {
  bool mtl4_only = false, drawable4_only = false, signal4_only = false, latency = false;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define IS(name) (!strcmp(a, name) && v && ++i)
    if (IS("--present") && (!strcmp(v, "mtl3") || !strcmp(v, "mtl4") || !strcmp(v, "mtl3-on-mtl4")))
      opt.mtl4 = strcmp(v, "mtl3") != 0, opt.mtl3_present = !strcmp(v, "mtl3-on-mtl4");
    else if (IS("--layer-set") && (!strcmp(v, "buffer") || !strcmp(v, "queue")))
      opt.set_on_queue = !strcmp(v, "queue"), drawable4_only |= opt.set_on_queue;
    else if (IS("--empty-commit") && (!strcmp(v, "on") || !strcmp(v, "off")))
      opt.empty_commit = !strcmp(v, "on"), mtl4_only |= !opt.empty_commit;
    else if (IS("--present-thread") && (!strcmp(v, "encode") || !strcmp(v, "queue")))
      opt.present_on_queue = !strcmp(v, "queue"), drawable4_only |= opt.present_on_queue;
    else if (IS("--present-at") && (!strcmp(v, "signal") || !strcmp(v, "done")))
      opt.present_at_done = !strcmp(v, "done"), signal4_only = true;
    else if (IS("--wait-for-drawable") && (!strcmp(v, "on") || !strcmp(v, "off")))
      opt.wait_for_drawable = !strcmp(v, "on"), signal4_only = true;
    else if (IS("--hud-lines") && (!strcmp(v, "off") || !strcmp(v, "on")))
      opt.hud_lines = !strcmp(v, "on");
    else if (IS("--encode-thread") && (!strcmp(v, "interactive") || !strcmp(v, "wine")))
      opt.encode_wine = !strcmp(v, "wine");
    else if (IS("--layer") && (!strcmp(v, "plain") || !strcmp(v, "wine")))
      opt.layer_wine = !strcmp(v, "wine");
    else if (IS("--framebuffer-only") && (!strcmp(v, "off") || !strcmp(v, "on")))
      opt.framebuffer_only = !strcmp(v, "on");
    else if (IS("--colorspace") && (!strcmp(v, "srgb") || !strcmp(v, "none")))
      opt.colorspace_none = !strcmp(v, "none");
    else if (IS("--transaction") && (!strcmp(v, "off") || !strcmp(v, "on")))
      opt.transaction = !strcmp(v, "on");
    else if (IS("--drawable") && (!strcmp(v, "next") || !strcmp(v, "link")))
      opt.link = !strcmp(v, "link");
    else if (IS("--link-latency") && (!strcmp(v, "1") || !strcmp(v, "2"))) // Apple takes only these
      opt.link_latency = atoi(v), latency = true;
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
  if (signal4_only && (!opt.mtl4 || opt.mtl3_present)) {
    printf("present_cursor_test: --present-at and --wait-for-drawable are --present mtl4's\n");
    return false;
  }
  if ((mtl4_only || drawable4_only) && !opt.mtl4) {
    printf("present_cursor_test: --layer-set, --empty-commit and --present-thread are Metal 4 variants\n");
    return false;
  }
  if (drawable4_only && opt.mtl3_present) {
    printf("present_cursor_test: --layer-set and --present-thread are --present mtl4's; mtl3-on-mtl4 has neither\n");
    return false;
  }
  if ((opt.transaction || opt.link) && opt.mtl3_present) {
    printf("present_cursor_test: --transaction and --drawable are --present mtl3's and mtl4's; mtl3-on-mtl4 has "
           "neither\n");
    return false;
  }
  if (opt.transaction && (opt.present_at_done || opt.present_on_queue)) {
    printf("present_cursor_test: --transaction on presents without a time from the encoding thread; "
           "--present-at done and --present-thread queue are --present mtl4's timed present\n");
    return false;
  }
  if (latency && !opt.link) {
    printf("present_cursor_test: --link-latency is --drawable link's\n");
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
  // Apple: 'unkn' (kDisplayVendorIDUnknown) when the display has no vendor id, 0xFFFFFFFF when no monitor
  // is behind the display id. A virtual display may carry any vendor id its maker gives it, so
  // "external" doesn't prove a monitor: the path status in every line decides.
  bool no_vendor = vendor == 0 || vendor == 0x756e6b6eu || vendor == 0xffffffffu;
  const char *kind = CGDisplayIsBuiltin(display) ? "built-in"
                     : no_vendor ? "unknown: no vendor id or no monitor (maybe a virtual display, which may not "
                                   "show #223)"
                                 : "external, by its vendor id (a virtual display can have one too)";
  printf("display: %u \"%s\", %s, vendor 0x%x model 0x%x, mode %zux%zu pixels at %.2f Hz, %.0fx%.0f points at "
         "scale %.1f, %s, %u active display(s)\n",
         display, screen.localizedName.UTF8String, kind, vendor, model, pw, ph, 1.0 / g_period, screen.frame.size.width,
         screen.frame.size.height, screen.backingScaleFactor,
         CGDisplayIsInMirrorSet(display) ? "mirrored" : "not mirrored", displays);
  printf("cursor: nothing public says whether the pointer is a hardware plane here; a window server sample "
         "beside the run shows it (cursor_callback, set_cursor_surface)\n");
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
  g_frame_event = [g_device newSharedEvent];
  g_shown_event = [g_device newSharedEvent];
  g_drawn_event = [g_device newSharedEvent];
  if (!g_frame_event || !g_shown_event || !g_drawn_event)
    return false;
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
      printf("skip: no Metal 4 on %s (--present %s)\n", g_device.name.UTF8String,
             opt.mtl3_present ? "mtl3-on-mtl4" : "mtl4");
      return 77;
    }
    if (opt.wave && opt.move_event && !CGPreflightPostEventAccess()) {
      printf("skip: --move event needs the Accessibility permission for this terminal\n");
      return 77;
    }

    printf("present_cursor_test: present=%s", present_name());
    if (opt.mtl4 && !opt.mtl3_present)
      printf(" layer-set=%s present-thread=%s", opt.set_on_queue ? "queue" : "buffer",
             opt.present_on_queue ? "queue" : "encode");
    if (opt.mtl4)
      printf(" empty-commit=%s", opt.empty_commit ? "on" : "off");
    if (opt.mtl4 && !opt.mtl3_present)
      printf(" present-at=%s wait-for-drawable=%s", opt.present_at_done ? "done" : "signal",
             opt.wait_for_drawable ? "on" : "off");
    printf(" hud-lines=%s encode-thread=%s layer=%s", opt.hud_lines ? "on" : "off",
           opt.encode_wine ? "wine" : "interactive", opt.layer_wine ? "wine" : "plain");
    // #245's, when on, so a run without them prints the line it printed before
    if (opt.framebuffer_only)
      printf(" framebuffer-only=on");
    if (opt.colorspace_none)
      printf(" colorspace=none");
    if (opt.transaction)
      printf(" transaction=on (plain present, no time)");
    if (opt.link)
      printf(" drawable=link link-latency=%d", opt.link_latency);
    NSString *bundle = NSBundle.mainBundle.bundleIdentifier;
    if (bundle)
      printf(" bundle=%s", bundle.UTF8String);
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
    if (opt.layer_wine) {
      // WineContentView's newMetalViewWithDevice: the Metal view as its lowest subview
      CGFloat scale = window.backingScaleFactor;
      g_window_image = black_image((size_t)(bounds.size.width * scale), (size_t)(bounds.size.height * scale));
      PCImageView *content = [[PCImageView alloc] initWithFrame:bounds];
      content.wantsLayer = YES;
      window.contentView = content;
      view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
      view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      content.autoresizesSubviews = YES;
      [content addSubview:view positioned:NSWindowBelow relativeTo:nil];
      content.needsDisplay = YES;
    } else {
      window.contentView = view;
    }
    window.contentView.superview.layer.backgroundColor = NULL; // athei/mtld3d #1123
    g_layer = (CAMetalLayer *)view.layer;
    g_layer.device = g_device;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = opt.framebuffer_only;
    g_layer.displaySyncEnabled = YES;
    g_layer.opaque = YES;
    if (!opt.colorspace_none) {
      CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      g_layer.colorspace = srgb;
      CGColorSpaceRelease(srgb);
      g_layer.wantsExtendedDynamicRangeContent = NO;
    }
    if (opt.transaction)
      g_layer.presentsWithTransaction = YES;
    g_layer.contentsScale = window.backingScaleFactor;
    g_layer.drawableSize = [view convertSizeToBacking:bounds.size];
    CGSize size = g_layer.drawableSize;
    printf("layer: %.0fx%.0f BGRA8Unorm, framebufferOnly %d, displaySyncEnabled %d, maximumDrawableCount %lu, "
           "opaque %d, %s, contentsScale %.1f\n",
           size.width, size.height, g_layer.framebufferOnly, g_layer.displaySyncEnabled,
           (unsigned long)g_layer.maximumDrawableCount, g_layer.opaque,
           g_layer.colorspace ? "sRGB" : "no colorspace", g_layer.contentsScale);
    if (opt.transaction)
      printf("layer: presentsWithTransaction %d; each present is the drawable's plain present in a "
             "CATransaction on the encoding thread, after %s\n",
             g_layer.presentsWithTransaction,
             opt.mtl4 ? "signalDrawable and a shared event the queue signals after it" : "waitUntilScheduled");
    if (opt.link)
      printf("drawable: from a CAMetalDisplayLink on the encoding thread's run loop, preferredFrameLatency %d, at "
             "%.2f Hz; a frame starts when the link hands its drawable out (DXMT takes the drawable after the "
             "frame); every present is the plain one, timed by the link\n",
             opt.link_latency, 1.0 / g_period);
    if (opt.layer_wine)
      printf("layer: wine: magnificationFilter %s, background black, under a content view with a %zux%zu image%s\n",
             g_layer.magnificationFilter.UTF8String, g_window_image ? CGImageGetWidth(g_window_image) : 0,
             g_window_image ? CGImageGetHeight(g_window_image) : 0, g_window_image ? "" : " (none: no bitmap)");
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
    if (opt.hud_lines)
      hud_setup();
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
      if (!opt.encode_wine) // a Wine thread has no QoS class
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
      pthread_create(&g_render, &attr, render_main, NULL);
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
