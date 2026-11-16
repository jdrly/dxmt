// gw2-test: metal
// gw2-build: U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
// gw2-build: (( ${#U} >= 1 ))
// gw2-build: [[ " ${U[*]} " == *" winemetal_unix.c "* ]]
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   for f in $U; do clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -O2 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Wno-unused-function -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $T/arc_u_${f:r}_$arch.o; done
// gw2-build:   clang -c -std=gnu17 -O2 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal src/winemetal/winemetal_thunks.c -o $T/arc_thunks_$arch.o
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c -o $T/arc_airconv_absent_$arch.o
// gw2-build:   clang -std=gnu17 -fno-objc-arc -O1 -Wall -arch $arch -mmacosx-version-min=26.0 -Isrc/nativemetal -Isrc/winemetal tests/gw2/autorelease_count_test.m $T/arc_*_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -o ${OUT}_$arch
// gw2-build: done
// gw2-run: for arch in x86_64 arm64; do OBJC_DEBUG_MISSING_POOLS=YES ${OUT}_$arch; done
//
// gw2 (#252): counts what the bridge's creation calls autorelease on a thread without an autorelease
// pool. GW2's render thread is a Wine thread with none, and it creates buffers, textures and views
// through the bridge; whatever Metal autoreleases inside those calls goes onto the thread's implicit
// pool page and lives until the thread exits. The calls go through the real bridge, as the game makes
// them: the PE side (winemetal_thunks.c) and the whole unix side linked natively with DXMT_NATIVE, each
// call reaching __wine_unix_call_funcs by its manifest number (as paired_replay_test). No Wine, no game.
//
// The runtime does the counting: with OBJC_DEBUG_MISSING_POOLS=YES it writes one "MISSING POOLS" line
// to stderr for each object autoreleased with no pool in place, naming the thread and the class. The
// test points stderr at a file and counts the lines of its worker thread (a plain pthread, never given
// a pool) around each call.
//
// On arm64 the runtime doesn't autorelease at once: -autorelease parks the object in a thread-local
// slot (objc4's ReturnAutoreleaseInfo), and it reaches the pool, or the MISSING POOLS line, only at the
// thread's next autorelease or pool push. So each counted window ends with an autorelease of a
// sentinel object, which pushes out whatever the call left parked; the sentinel's own line comes in a
// later window and isn't counted. x86_64 doesn't park, and there the sentinel's line comes at once.
//
// Two controls come first and must each count exactly one: an object autoreleased on the fresh
// thread (on arm64 a parked one, so it checks the sentinel too), and one autoreleased after a pool
// was pushed and popped there (Metal may push and pop pools of its own inside a call, and an object
// autoreleased after that must still be seen). Either control failing fails the test: the counts
// below would mean nothing.
//
// gw2 (#269): then the release. A test object whose -dealloc autoreleases one object stands in for a
// driver that autoreleases on release (the M1's AGXG13GDevice, the runner's AppleParavirtDevice):
// released with a plain -release it must count one (a dealloc's autorelease is seen), released
// through the bridge's NSObject_release it must count none. That second check doesn't depend on the
// device or its class. Then a pool released through NSObject_release, as WMT::MakeAutoreleasePool's
// Reference releases it, must drain its object and return: had the release pushed a pool of its own
// around it, that pool's pop would come after the drain took it away, and the runtime aborts.
//
// Each call runs once cold, then 100 times; each object is released (and its cleanup done) outside
// the counted window, and that release is counted on its own. One RESULT line per call and arch:
//
//   RESULT x86_64 newBuffer shared: first 0, then 0 in 100; release 0 in 101
//
// with the classes the runtime named, if any. gw2 (#269): a release count other than 0 fails the
// test (each release goes through the bridge's NSObject_release, which drains a pool of its own); the
// creation counts only report. The calls are those the D3D11 front end makes when the
// game creates a resource or state object (dxmt_buffer.cpp, dxmt_texture.cpp, dxmt_sampler.cpp,
// d3d11_state_object.cpp), plus the residency set add that follows a buffer or texture creation
// (dxmt_gw2_residency.cpp), skipped where the device gives no residency set. Pipelines and libraries
// are created on DXMT's own threads, which drain a pool per task, so they aren't here. Neither are the
// heap calls (no heap is created on GW2's path), nor queues, shared events, events, fences and
// upscalers: they come with queues, contexts, upscalers, keyed-mutex textures and ID3D11Fence objects,
// not with each buffer, texture or view. Without a Metal device it exits 77.

#import <Foundation/Foundation.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "winemetal.h"

#if defined(__x86_64__)
static const char *kArch = "x86_64";
#else
static const char *kArch = "arm64";
#endif

enum { kRepeat = 100 };

static int capture_fd = -1;    // the file stderr points at
static char thread_tag[32];    // "(0x...)": how the runtime names the worker thread
static int failures = 0;

// autoreleased at the end of each counted window; its lines aren't counted
@interface AutoreleaseCountSentinel : NSObject
@end
@implementation AutoreleaseCountSentinel
@end

// gw2 (#269): autoreleases one object in its -dealloc, as the drivers above do on a resource's release
@interface AutoreleaseOnDealloc : NSObject
@end
@implementation AutoreleaseOnDealloc
- (void)dealloc {
  [[[NSObject alloc] init] autorelease];
  [super dealloc];
}
@end

static off_t
captured(void) {
  struct stat st;
  return fstat(capture_fd, &st) == 0 ? st.st_size : 0;
}

// The worker's MISSING POOLS lines written since `from`, after pushing out what's parked (see above);
// their class names are appended to `classes`
static int
count_since(off_t from, char *classes, size_t classes_size) {
  [[[AutoreleaseCountSentinel alloc] init] autorelease];
  off_t to = captured();
  if (to <= from)
    return 0;
  size_t n = (size_t)(to - from);
  char *buf = malloc(n + 1);
  ssize_t got = pread(capture_fd, buf, n, from);
  buf[got > 0 ? got : 0] = 0;
  int count = 0;
  for (char *line = buf; line && *line;) {
    char *end = strchr(line, '\n');
    if (end)
      *end = 0;
    if (strstr(line, "MISSING POOLS: ") && strstr(line, thread_tag) &&
        !strstr(line, "of class AutoreleaseCountSentinel ")) {
      count++;
      char *cls = strstr(line, "of class ");
      char *cls_end = cls ? strstr(cls, " autoreleased") : NULL;
      if (classes && cls && cls_end) {
        cls += strlen("of class ");
        char name[128];
        snprintf(name, sizeof(name), "%.*s", (int)(cls_end - cls), cls);
        if (!strstr(classes, name) && strlen(classes) + strlen(name) + 2 < classes_size) {
          if (*classes)
            strcat(classes, " ");
          strcat(classes, name);
        }
      }
    }
    line = end ? end + 1 : NULL;
  }
  free(buf);
  return count;
}

struct fixture {
  obj_handle_t device;
  obj_handle_t texture;       // the base of the views: RGBA8, 256x256, 9 levels, PixelFormatView
  obj_handle_t linear_buffer; // the base of the buffer textures
  obj_handle_t residency_set; // 0 where the device gives none
  obj_handle_t resident;      // the allocation added to it
};

struct created {
  obj_handle_t obj;
  void *memory;        // CPU-placed memory to free after the release
  mach_port_t port;    // a shared texture's mach port
};

typedef struct created (*create_fn)(struct fixture *);

static struct WMTTextureInfo
texture_info(enum WMTPixelFormat format, uint32_t width, uint32_t height, uint32_t levels, uint32_t usage,
             uint64_t options) {
  struct WMTTextureInfo info;
  memset(&info, 0, sizeof(info));
  info.pixel_format = format;
  info.width = width;
  info.height = height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = levels;
  info.sample_count = 1;
  info.usage = usage;
  info.options = options;
  return info;
}

static struct created
new_buffer(struct fixture *f, uint64_t options, bool placed) {
  struct WMTBufferInfo info;
  memset(&info, 0, sizeof(info));
  info.length = 64 * 1024;
  info.options = options;
  struct created c = {0};
  if (placed) {
    c.memory = aligned_alloc(16384, info.length);
    WMT_MEMPTR_SET(info.memory, c.memory);
  }
  c.obj = MTLDevice_newBuffer(f->device, &info);
  return c;
}

static struct created
c_buffer_shared(struct fixture *f) {
  return new_buffer(f, WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked, false);
}

static struct created
c_buffer_private(struct fixture *f) {
  return new_buffer(f, WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked, false);
}

static struct created
c_buffer_placed(struct fixture *f) {
  return new_buffer(f, WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked, true);
}

static struct created
c_texture(struct fixture *f) {
  struct WMTTextureInfo info = texture_info(
      WMTPixelFormatRGBA8Unorm, 256, 256, 9, WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget,
      WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked
  );
  return (struct created){.obj = MTLDevice_newTexture(f->device, &info)};
}

static struct created
c_buffer_texture(struct fixture *f) {
  struct WMTTextureInfo info = texture_info(
      WMTPixelFormatR32Float, 4096, 1, 1, WMTTextureUsageShaderRead,
      WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked
  );
  info.type = WMTTextureTypeTextureBuffer;
  return (struct created){.obj = MTLBuffer_newTexture(f->linear_buffer, &info, 0, 4096 * 4)};
}

static struct created
c_texture_view(struct fixture *f) {
  struct WMTTextureSwizzleChannels identity = {
      WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha
  };
  uint64_t gpu_resource_id = 0;
  obj_handle_t view = MTLTexture_newTextureView(
      f->texture, WMTPixelFormatRGBA8Unorm_sRGB, WMTTextureType2D, 0, 9, 0, 1, identity, &gpu_resource_id
  );
  return (struct created){.obj = view};
}

static struct created
c_shared_texture(struct fixture *f) {
  struct WMTTextureInfo info = texture_info(
      WMTPixelFormatRGBA8Unorm, 64, 64, 1, WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget,
      WMTResourceStorageModePrivate
  );
  struct created c = {.obj = MTLDevice_newSharedTexture(f->device, &info)};
  c.port = info.mach_port;
  return c;
}

static struct created
c_sampler(struct fixture *f) {
  struct WMTSamplerInfo info;
  memset(&info, 0, sizeof(info));
  info.min_filter = WMTSamplerMinMagFilterLinear;
  info.mag_filter = WMTSamplerMinMagFilterLinear;
  info.mip_filter = WMTSamplerMipFilterLinear;
  info.r_address_mode = info.s_address_mode = info.t_address_mode = WMTSamplerAddressModeRepeat;
  info.compare_function = WMTCompareFunctionNever;
  info.lod_max_clamp = 1000.0f;
  info.max_anisotroy = 1;
  info.normalized_coords = true;
  info.support_argument_buffers = true;
  return (struct created){.obj = MTLDevice_newSamplerState(f->device, &info)};
}

static struct created
c_depth_stencil(struct fixture *f) {
  struct WMTDepthStencilInfo info;
  memset(&info, 0, sizeof(info));
  info.depth_compare_function = WMTCompareFunctionLess;
  info.depth_write_enabled = true;
  return (struct created){.obj = MTLDevice_newDepthStencilState(f->device, &info)};
}

// not a creation: the add that follows one (gw2::residencyAdd); its "release" is the remove
static struct created
c_residency_add(struct fixture *f) {
  MTLResidencySet_addAllocations(f->residency_set, &f->resident, 1);
  return (struct created){0};
}

static void
release(struct fixture *f, create_fn fn, struct created c) {
  if (fn == c_residency_add) {
    MTLResidencySet_removeAllocations(f->residency_set, &f->resident, 1);
    return;
  }
  if (c.obj)
    NSObject_release(c.obj);
  if (c.port)
    mach_port_deallocate(mach_task_self(), c.port);
  free(c.memory);
}

static void
measure(struct fixture *f, const char *name, create_fn fn) {
  if (fn == c_residency_add && !f->residency_set) {
    printf("RESULT %s %s: skipped, the device gave no residency set\n", kArch, name);
    return;
  }
  char classes[512] = "";
  int first = 0, then = 0, released = 0, null = 0;
  for (int i = 0; i <= kRepeat; i++) {
    off_t mark = captured();
    struct created c = fn(f);
    int n = count_since(mark, classes, sizeof(classes));
    if (i == 0)
      first = n;
    else
      then += n;
    if (!c.obj && fn != c_residency_add)
      null++;
    mark = captured();
    release(f, fn, c);
    released += count_since(mark, classes, sizeof(classes));
  }
  if (null) {
    printf("FAIL %s %s: %d of %d calls returned no object\n", kArch, name, null, kRepeat + 1);
    failures++;
  }
  if (released) {
    printf("FAIL %s %s: %d of %d releases left an object autoreleased with no pool\n", kArch, name, released,
           kRepeat + 1);
    failures++;
  }
  printf("RESULT %s %s: first %d, then %d in %d; release %d in %d%s%s\n", kArch, name, first, then, kRepeat,
         released, kRepeat + 1, *classes ? "; classes: " : "", classes);
}

static void *
worker(void *arg) {
  struct fixture *f = arg;
  snprintf(thread_tag, sizeof(thread_tag), "(%p)", (void *)pthread_self());

  // control 1: an object autoreleased on a thread that never had a pool
  off_t mark = captured();
  [[[NSObject alloc] init] autorelease];
  int fresh = count_since(mark, NULL, 0);
  // control 2: the same after a pool was pushed and popped on it
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  [[[NSObject alloc] init] autorelease];
  [pool drain];
  mark = captured();
  [[[NSObject alloc] init] autorelease];
  int after_pop = count_since(mark, NULL, 0);
  printf("control %s: %d on a fresh thread, %d after a pool's pop (each must be 1)\n", kArch, fresh, after_pop);
  if (fresh != 1 || after_pop != 1) {
    printf("FAIL %s: the runtime's MISSING POOLS lines don't count this thread's autoreleases\n", kArch);
    failures++;
    return NULL;
  }

  // gw2 (#269): a release whose dealloc autoreleases, plain and through the bridge
  id obj = [[AutoreleaseOnDealloc alloc] init];
  mark = captured();
  [obj release];
  int plain = count_since(mark, NULL, 0);
  obj = [[AutoreleaseOnDealloc alloc] init];
  mark = captured();
  NSObject_release((obj_handle_t)obj);
  int bridged = count_since(mark, NULL, 0);
  printf("release %s: %d after a plain release (must be 1), %d after NSObject_release (must be 0)\n", kArch, plain,
         bridged);
  if (plain != 1) {
    printf("FAIL %s: the runtime's MISSING POOLS lines don't count what a dealloc autoreleases\n", kArch);
    failures++;
    return NULL;
  }
  if (bridged) {
    printf("FAIL %s: NSObject_release left what the dealloc autoreleased with no pool\n", kArch);
    failures++;
  }
  // gw2 (#269): a pool from the bridge, released through it with an object in it
  obj_handle_t outer = NSAutoreleasePool_alloc_init();
  [[[NSObject alloc] init] autorelease];
  mark = captured();
  NSObject_release(outer);
  int drained = count_since(mark, NULL, 0);
  printf("release %s: %d after NSObject_release of a pool (must be 0)\n", kArch, drained);
  if (drained) {
    printf("FAIL %s: NSObject_release of a pool left its object with no pool\n", kArch);
    failures++;
  }

  measure(f, "newBuffer shared", c_buffer_shared);
  measure(f, "newBuffer private", c_buffer_private);
  measure(f, "newBuffer CPU-placed", c_buffer_placed);
  measure(f, "newTexture", c_texture);
  measure(f, "MTLBuffer newTexture", c_buffer_texture);
  measure(f, "newTextureView", c_texture_view);
  measure(f, "newSharedTexture", c_shared_texture);
  measure(f, "newSamplerState", c_sampler);
  measure(f, "newDepthStencilState", c_depth_stencil);
  measure(f, "ResidencySet addAllocations", c_residency_add);
  return NULL;
}

int
main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *debug = getenv("OBJC_DEBUG_MISSING_POOLS");
  if (!debug || strcmp(debug, "YES")) {
    printf("FAIL: run it with OBJC_DEBUG_MISSING_POOLS=YES\n");
    return 1;
  }

  // the setup runs on the main thread, in a pool; only the worker has none
  obj_handle_t pool = NSAutoreleasePool_alloc_init();
  obj_handle_t devices = WMTCopyAllDevices();
  if (!devices || !NSArray_count(devices)) {
    printf("SKIP: no Metal device\n");
    return 77;
  }
  struct fixture f = {0};
  f.device = NSArray_object(devices, 0);
  NSObject_retain(f.device);
  char name[256] = {0};
  NSString_getCString(MTLDevice_name(f.device), name, sizeof(name), WMTUTF8StringEncoding);
  printf("device: %s\n", name);

  struct WMTTextureInfo tinfo = texture_info(
      WMTPixelFormatRGBA8Unorm, 256, 256, 9,
      WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget | WMTTextureUsagePixelFormatView,
      WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked
  );
  f.texture = MTLDevice_newTexture(f.device, &tinfo);
  struct WMTBufferInfo binfo;
  memset(&binfo, 0, sizeof(binfo));
  binfo.length = 4096 * 4;
  binfo.options = WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked;
  f.linear_buffer = MTLDevice_newBuffer(f.device, &binfo);
  obj_handle_t err = 0;
  f.residency_set = MTLDevice_newResidencySet(f.device, 64, &err);
  f.resident = f.linear_buffer;
  if (!f.texture || !f.linear_buffer) {
    printf("FAIL: the fixture's texture or buffer wasn't created\n");
    return 1;
  }
  NSObject_release(pool);

  // stderr into a file, which the worker reads back
  char path[] = "/tmp/autorelease_count.XXXXXX";
  capture_fd = mkstemp(path);
  if (capture_fd < 0) {
    printf("FAIL: mkstemp: %s\n", strerror(errno));
    return 1;
  }
  unlink(path);
  fflush(stderr);
  int saved_stderr = dup(2);
  dup2(capture_fd, 2);

  pthread_t thread;
  pthread_create(&thread, NULL, worker, &f);
  pthread_join(thread, NULL);

  fflush(stderr);
  dup2(saved_stderr, 2);
  close(saved_stderr);
  if (failures) {
    // what the runtime wrote, to see why
    char buf[4096];
    ssize_t n = pread(capture_fd, buf, sizeof(buf) - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    printf("stderr while the worker ran (first %zd bytes):\n%s\n", n > 0 ? n : 0, buf);
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("autorelease count %s: OK\n", kArch);
  return 0;
}
