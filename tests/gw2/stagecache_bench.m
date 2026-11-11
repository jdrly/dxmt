// gw2 (#159): how much 2, 3 or 4 threads shorten warming's plan on a cold Metal cache. A native
// benchmark for the Mac, not a tests/gw2/run.zsh test: it needs the game's recorded recipes and AIR.
//
//   clang -x objective-c -fobjc-arc -O1 -arch x86_64 tests/gw2/stagecache_bench.m -framework Metal \
//     -framework Foundation -lsqlite3 -o stagecache
//   ./stagecache <recipes_1.db> <shaders_320.db> [threads] [first] [count] [qos]
//
// threads: 1 (default) to 16. first, count: the R rows replayed (default 0 and all). qos: init
// (default, user-initiated), ui, default, utility, background, or none (as a Wine thread: QoS opted
// out through the pthread priority API). x86_64, so Metal runs under Rosetta as it does for DXMT.
//
// It replays the recorded recipes (recipes_1.db, R rows in rowid order) with their real AIR
// (shaders_320.db, cache_26) on the real device, as warming does (W2/W3): first every function the
// recipes use (the AIR read, newLibrary, newFunction: WarmVariantJob), then every pipeline
// (newRenderPipelineState: WarmPipelineJob). Each phase runs on `threads` threads that take the next
// item in plan order. RESULT's wall= is the plan's wall time; the issue compares it across 1 to 4.
//
// Each build is put in a class by the recipes before it in plan order (with more than one thread, an
// earlier recipe may still be building):
//   V0/V1   the vertex function was (not) part of an earlier recipe
//   P0      the pixel function was never part of an earlier recipe
//   P1s     it was, with the same fragment state (formats, blending, sample count, depth/stencil)
//   P1d     it was, only with another fragment state
//   Pn      no pixel shader
//
// Both databases are opened read-only and immutable (no -shm or -wal is created; a WAL the game left
// isn't read, so close the game first). No Wine, no game. Metal caches what it compiles for an
// unbundled command-line tool in $(getconf DARWIN_USER_CACHE_DIR)com.apple.metal/16777235_355/ (not the
// game's cache): delete that folder before each cold run. A rerun without deleting it is a warm run.
//
// Started from the perf review's scratch-C/stagecache.m (GW2 on Mac workspace, 2026-11-10), which
// had one thread and no phases.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <sys/qos.h>

static mach_timebase_info_data_t g_tb; // set in main, before any thread

static double
now_ms(void) {
  return mach_absolute_time() * (double)g_tb.numer / g_tb.denom / 1e6;
}

typedef struct {
  const uint8_t *p, *end;
  int ok;
} Rd;
static uint32_t
u32(Rd *r) {
  if (r->end - r->p < 4) {
    r->ok = 0;
    return 0;
  }
  uint32_t v = r->p[0] | r->p[1] << 8 | r->p[2] << 16 | (uint32_t)r->p[3] << 24;
  r->p += 4;
  return v;
}
static uint8_t
u8(Rd *r) {
  if (r->end - r->p < 1) {
    r->ok = 0;
    return 0;
  }
  return *r->p++;
}
static NSData *
dig(Rd *r) {
  if (r->end - r->p < 20) {
    r->ok = 0;
    return nil;
  }
  NSData *d = [NSData dataWithBytes:r->p length:20];
  r->p += 20;
  return d;
}

static NSString *
hex(NSData *d) {
  NSMutableString *s = [NSMutableString string];
  const uint8_t *b = d.bytes;
  for (NSUInteger i = 0; i < d.length; i++)
    [s appendFormat:@"%02x", b[i]];
  return s;
}

static sqlite3 *
open_ro(const char *path) {
  sqlite3 *db = NULL;
  NSString *uri = [NSString
      stringWithFormat:@"file:%@?mode=ro&immutable=1",
                       [[NSString stringWithUTF8String:path]
                           stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet]];
  if (sqlite3_open_v2(uri.UTF8String, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX, NULL) !=
      SQLITE_OK) {
    fprintf(stderr, "open %s: %s\n", path, sqlite3_errmsg(db));
    exit(1);
  }
  return db;
}

static int
cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

enum { V0P0, V1P0, V0P1s, V1P1s, V0P1d, V1P1d, V0Pn, V1Pn, NCLASS };
static const char *g_class_names[NCLASS] = {"V0P0", "V1P0", "V0P1s", "V1P1s", "V0P1d", "V1P1d", "V0Pn", "V1Pn"};

typedef struct {
  // the plan (read-only while the threads run)
  const char *shaders_path;
  qos_class_t qos;
  int use_qos;
  id<MTLDevice> __unsafe_unretained device;
  NSArray *__unsafe_unretained function_keys, *__unsafe_unretained function_names;
  NSArray *__unsafe_unretained descriptors; // per recipe
  const int *vs_index, *ps_index;           // per recipe: indices into the functions (ps: -1 if none)
  int functions, recipes;
  // results: each slot written by the one thread that took its index
  void **function_objects; // CFBridgingRetain'd id<MTLFunction>, NULL when it has no AIR or failed
  double *function_ms, *build_ms; // build_ms: -1 failed, -2 a function missing
  _Atomic int next;
  int phase; // 0: functions, 1: pipelines
} Work;

static void
apply_qos(Work *w) {
  if (!w->use_qos) {
    struct sched_param sp;
    int policy;
    pthread_getschedparam(pthread_self(), &policy, &sp);
    pthread_setschedparam(pthread_self(), policy, &sp);
  } else {
    pthread_set_qos_class_self_np(w->qos, 0);
  }
}

static void *
worker(void *arg) {
  Work *w = arg;
  apply_qos(w);
  if (w->phase == 0) {
    // each thread its own connection (an immutable read-only database needs no locking between them)
    sqlite3 *adb = open_ro(w->shaders_path);
    sqlite3_stmt *get;
    if (sqlite3_prepare_v2(adb, "SELECT value FROM cache_26 WHERE key = ?;", -1, &get, NULL) != SQLITE_OK) {
      fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(adb));
      exit(1);
    }
    for (int i; (i = atomic_fetch_add(&w->next, 1)) < w->functions;) {
      @autoreleasepool {
        NSData *k = w->function_keys[i];
        double f0 = now_ms();
        sqlite3_reset(get);
        sqlite3_bind_blob(get, 1, k.bytes, (int)k.length, SQLITE_STATIC);
        if (sqlite3_step(get) != SQLITE_ROW)
          continue; // no AIR: its recipes count as noair
        dispatch_data_t data = dispatch_data_create(sqlite3_column_blob(get, 0), sqlite3_column_bytes(get, 0), nil,
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *err = nil;
        id<MTLLibrary> lib = [w->device newLibraryWithData:data error:&err];
        id<MTLFunction> f = [lib newFunctionWithName:w->function_names[i]];
        w->function_ms[i] = now_ms() - f0;
        if (f)
          w->function_objects[i] = (void *)CFBridgingRetain(f);
      }
    }
    sqlite3_finalize(get);
    sqlite3_close(adb);
    return NULL;
  }
  for (int i; (i = atomic_fetch_add(&w->next, 1)) < w->recipes;) {
    @autoreleasepool {
      void *vs = w->function_objects[w->vs_index[i]];
      void *ps = w->ps_index[i] >= 0 ? w->function_objects[w->ps_index[i]] : NULL;
      if (!vs || (w->ps_index[i] >= 0 && !ps)) {
        w->build_ms[i] = -2;
        continue;
      }
      MTLRenderPipelineDescriptor *d = w->descriptors[i]; // this thread's alone
      d.vertexFunction = (__bridge id<MTLFunction>)vs;
      d.fragmentFunction = ps ? (__bridge id<MTLFunction>)ps : nil;
      NSError *err = nil;
      double b0 = now_ms();
      id<MTLRenderPipelineState> pso = [w->device newRenderPipelineStateWithDescriptor:d error:&err];
      double ms = now_ms() - b0;
      w->build_ms[i] = pso ? ms : -1;
    }
  }
  return NULL;
}

// Runs one phase on `threads` threads; returns its wall time in ms.
static double
run_phase(Work *w, int phase, int threads) {
  w->phase = phase;
  atomic_store(&w->next, 0);
  pthread_t t[16];
  double t0 = now_ms();
  for (int i = 0; i < threads; i++)
    pthread_create(&t[i], NULL, worker, w);
  for (int i = 0; i < threads; i++)
    pthread_join(t[i], NULL);
  return now_ms() - t0;
}

int
main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 3) {
      fprintf(stderr, "usage: %s <recipes_1.db> <shaders_320.db> [threads] [first] [count] [qos]\n", argv[0]);
      return 2;
    }
    mach_timebase_info(&g_tb);
    int threads = argc > 3 ? atoi(argv[3]) : 1;
    int first = argc > 4 ? atoi(argv[4]) : 0;
    int count = argc > 5 ? atoi(argv[5]) : INT32_MAX;
    const char *qos = argc > 6 ? argv[6] : "init";
    if (threads < 1 || threads > 16 || first < 0 || count < 1) {
      fprintf(stderr, "threads: 1 to 16; first >= 0; count >= 1\n");
      return 2;
    }
    Work w = {.shaders_path = argv[2], .qos = QOS_CLASS_USER_INITIATED, .use_qos = strcmp(qos, "none") != 0};
    if (!strcmp(qos, "ui"))
      w.qos = QOS_CLASS_USER_INTERACTIVE;
    else if (!strcmp(qos, "default"))
      w.qos = QOS_CLASS_DEFAULT;
    else if (!strcmp(qos, "utility"))
      w.qos = QOS_CLASS_UTILITY;
    else if (!strcmp(qos, "background"))
      w.qos = QOS_CLASS_BACKGROUND;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    device.shouldMaximizeConcurrentCompilation = YES; // as DXMT sets it
    printf("device=%s maximumConcurrentCompilationTaskCount=%lu\n", device.name.UTF8String,
           (unsigned long)device.maximumConcurrentCompilationTaskCount);
    w.device = device;

    // the plan: the recipes in rowid order, their functions once each in first-use order, the classes
    double r0 = now_ms();
    sqlite3 *rdb = open_ro(argv[1]);
    sqlite3_stmt *all;
    if (sqlite3_prepare_v2(rdb, "SELECT value FROM cache_1 WHERE substr(key,1,1) = x'52' ORDER BY rowid;", -1, &all,
                           NULL) != SQLITE_OK) {
      fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(rdb));
      return 1;
    }
    NSMutableArray<NSData *> *function_keys = [NSMutableArray array];
    NSMutableArray<NSString *> *function_names = [NSMutableArray array];
    NSMutableDictionary<NSData *, NSNumber *> *function_index = [NSMutableDictionary dictionary];
    NSMutableArray<MTLRenderPipelineDescriptor *> *descriptors = [NSMutableArray array];
    NSMutableSet<NSData *> *vs_seen = [NSMutableSet set], *ps_seen = [NSMutableSet set],
                           *ps_state_seen = [NSMutableSet set];
    int cap = 1024, recipes = 0, row = 0, bad = 0;
    int *vs_index = malloc(cap * sizeof(int)), *ps_index = malloc(cap * sizeof(int)), *cls = malloc(cap * sizeof(int));
    while (sqlite3_step(all) == SQLITE_ROW && recipes < count) {
      if (row++ < first)
        continue;
      @autoreleasepool {
        Rd r = {sqlite3_column_blob(all, 0), NULL, 1};
        r.end = r.p + sqlite3_column_bytes(all, 0);
        if (u32(&r) != 1) { // kRecipeSchema (dxmt_compile_recipe.cpp's serialize)
          bad++;
          continue;
        }
        u32(&r); // metal target
        NSData *vs_sha = dig(&r), *vs_var = dig(&r), *ps_sha = nil, *ps_var = nil;
        if (u8(&r)) {
          ps_sha = dig(&r);
          ps_var = dig(&r);
        }
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        const uint8_t *frag_begin = r.p;
        for (int i = 0; i < 8; i++) {
          MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
          c.pixelFormat = (MTLPixelFormat)(u32(&r) & ~(0xfffu << 12));
          c.rgbBlendOperation = (MTLBlendOperation)u32(&r);
          c.alphaBlendOperation = (MTLBlendOperation)u32(&r);
          c.sourceRGBBlendFactor = (MTLBlendFactor)u32(&r);
          c.destinationRGBBlendFactor = (MTLBlendFactor)u32(&r);
          c.sourceAlphaBlendFactor = (MTLBlendFactor)u32(&r);
          c.destinationAlphaBlendFactor = (MTLBlendFactor)u32(&r);
          c.writeMask = (MTLColorWriteMask)u8(&r);
          c.blendingEnabled = u8(&r);
        }
        d.alphaToCoverageEnabled = u8(&r);
        int logic_enabled = u8(&r);
        u32(&r); // logic operation (private API; GW2's recipes don't use it: checked below)
        d.rasterizationEnabled = u8(&r);
        d.rasterSampleCount = u8(&r);
        d.depthAttachmentPixelFormat = (MTLPixelFormat)(u32(&r) & ~(0xfffu << 12));
        d.stencilAttachmentPixelFormat = (MTLPixelFormat)(u32(&r) & ~(0xfffu << 12));
        const uint8_t *frag_end = r.p;
        uint32_t imm_v = u32(&r), imm_f = u32(&r);
        d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)u32(&r);
        u32(&r); // tessellation (GW2 has none)
        u8(&r);
        u32(&r);
        u32(&r);
        u8(&r); // indirect command buffers
        if (!r.ok || r.p != r.end || logic_enabled) {
          bad++;
          continue;
        }
        for (int i = 0; i < 31; i++) {
          if (imm_f & (1u << i))
            d.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
          if (imm_v & (1u << i))
            d.vertexBuffers[i].mutability = MTLMutabilityImmutable;
        }
        int index[2] = {-1, -1};
        NSData *keys[2] = {nil, nil};
        for (int s = 0; s < (ps_sha ? 2 : 1); s++) {
          NSMutableData *k = [NSMutableData dataWithData:s ? ps_sha : vs_sha];
          [k appendData:s ? ps_var : vs_var];
          keys[s] = k;
          NSNumber *n = function_index[k];
          if (!n) {
            n = @(function_keys.count);
            function_index[k] = n;
            [function_keys addObject:k];
            // as DXMT names it (gw2::warmFunctionName)
            [function_names addObject:[NSString stringWithFormat:@"%s_%@_%@", s ? "ps" : "vs",
                                                                 [hex(s ? ps_sha : vs_sha) substringToIndex:8],
                                                                 hex(s ? ps_var : vs_var)]];
          }
          index[s] = n.intValue;
        }
        if (recipes == cap) {
          cap *= 2;
          vs_index = realloc(vs_index, cap * sizeof(int));
          ps_index = realloc(ps_index, cap * sizeof(int));
          cls = realloc(cls, cap * sizeof(int));
        }
        int v1 = [vs_seen containsObject:keys[0]], c;
        if (ps_sha) {
          NSMutableData *ps_state = [NSMutableData dataWithData:keys[1]];
          [ps_state appendBytes:frag_begin length:frag_end - frag_begin];
          if ([ps_state_seen containsObject:ps_state])
            c = v1 ? V1P1s : V0P1s;
          else if ([ps_seen containsObject:keys[1]])
            c = v1 ? V1P1d : V0P1d;
          else
            c = v1 ? V1P0 : V0P0;
          [ps_seen addObject:keys[1]];
          [ps_state_seen addObject:ps_state];
        } else {
          c = v1 ? V1Pn : V0Pn;
        }
        [vs_seen addObject:keys[0]];
        vs_index[recipes] = index[0];
        ps_index[recipes] = index[1];
        cls[recipes] = c;
        [descriptors addObject:d];
        recipes++;
      }
    }
    sqlite3_finalize(all);
    sqlite3_close(rdb);
    double read_ms = now_ms() - r0;

    w.function_keys = function_keys;
    w.function_names = function_names;
    w.descriptors = descriptors;
    w.vs_index = vs_index;
    w.ps_index = ps_index;
    w.functions = (int)function_keys.count;
    w.recipes = recipes;
    w.function_objects = calloc(w.functions ? w.functions : 1, sizeof(void *));
    w.function_ms = calloc(w.functions ? w.functions : 1, sizeof(double));
    w.build_ms = calloc(recipes ? recipes : 1, sizeof(double));

    printf("threads=%d qos=%s first=%d recipes=%d functions=%d bad=%d read=%.0f ms\n", threads, qos, first, recipes,
           w.functions, bad, read_ms);
    double fn_wall = run_phase(&w, 0, threads);
    double pso_wall = run_phase(&w, 1, threads);

    // functions
    int fn_n = 0, noair_fn = 0;
    double fn_sum = 0;
    for (int i = 0; i < w.functions; i++) {
      if (w.function_objects[i]) {
        fn_n++;
        fn_sum += w.function_ms[i];
      } else {
        noair_fn++;
      }
    }
    printf("functions: %d made, %d without AIR or failed, %.3f ms each, wall %.2f s\n", fn_n, noair_fn,
           fn_n ? fn_sum / fn_n : 0, fn_wall / 1000);

    // pipelines, by class
    double *t[NCLASS];
    int n[NCLASS] = {0}, built = 0, failed = 0, noair = 0;
    double build_sum = 0;
    for (int c = 0; c < NCLASS; c++)
      t[c] = calloc(recipes ? recipes : 1, sizeof(double));
    for (int i = 0; i < recipes; i++) {
      double ms = w.build_ms[i];
      if (ms == -2) {
        noair++;
      } else if (ms < 0) {
        failed++;
      } else {
        t[cls[i]][n[cls[i]]++] = ms;
        built++;
        build_sum += ms;
      }
    }
    printf("pipelines: %d built, %d failed, %d noair, %.2f ms each, wall %.2f s\n", built, failed, noair,
           built ? build_sum / built : 0, pso_wall / 1000);
    printf("%-6s %6s %8s %8s %8s %8s %9s\n", "class", "n", "mean", "median", "p90", "max", "sum ms");
    for (int c = 0; c < NCLASS; c++) {
      if (!n[c])
        continue;
      qsort(t[c], n[c], sizeof(double), cmpd);
      double sum = 0;
      for (int i = 0; i < n[c]; i++)
        sum += t[c][i];
      printf("%-6s %6d %8.2f %8.2f %8.2f %8.2f %9.0f\n", g_class_names[c], n[c], sum / n[c], t[c][n[c] / 2],
             t[c][(int)(n[c] * 0.9)], t[c][n[c] - 1], sum);
    }
    // the line for the issue: the plan's wall time at this thread count, and the builds' summed time
    printf("RESULT threads=%d wall=%.1f s functions=%.1f s pipelines=%.1f s built=%d buildsum=%.1f s\n", threads,
           (fn_wall + pso_wall) / 1000, fn_wall / 1000, pso_wall / 1000, built, build_sum / 1000);
    for (int i = 0; i < w.functions; i++)
      if (w.function_objects[i])
        CFBridgingRelease(w.function_objects[i]);
  }
  return 0;
}
