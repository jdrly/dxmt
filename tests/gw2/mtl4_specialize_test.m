// gw2-test: metal
// gw2-build: for arch in x86_64 arm64; do clang -x objective-c -std=gnu17 -fobjc-arc -O1 -Wall -arch $arch -mmacosx-version-min=26.0 tests/gw2/mtl4_specialize_test.m -framework Metal -framework Foundation -lsqlite3 -o ${OUT}_$arch; done
// gw2-run: ${OUT}_x86_64 && ${OUT}_arm64
//
// gw2 (#160): what a Metal 4 specialization costs against a full fragment compile. A pipeline built
// through MTL4Compiler can leave its color attachments' pixel format, write mask and blend state
// unspecialized, and get them later from newRenderPipelineStateBySpecializationWithDescriptor:
// pipeline:error:. If that is cheap, DXMT could build the unspecialized pipeline as soon as both
// shaders are known and specialize it at the draw (#34). Not part of the build.
//
// Without arguments it is a test: two functions of its own, an unspecialized pipeline, one
// specialization; skipped (77) without Metal 4 (the CI runner). With arguments it is the benchmark,
// measured as the perf review's stagecache measured the full compiles (8.7 ms for another blend
// state): it replays recorded recipes (recipes_1.db, R rows in rowid order) with their real AIR
// (shaders_320.db, cache_26) on the real device, one at a time, x86_64 under Rosetta. For each recipe
// with a pixel shader and a first color attachment, all through one MTL4Compiler:
//   full     the recipe's pipeline
//   other    the same two functions with another blend state on attachment 0 (stagecache's
//            STAGE_VARIANTS "other blend": a full fragment compile of the same functions)
//   unspec   the same two functions, the color attachments' format, write mask and blend state
//            unspecialized
//   spec     unspec specialized to a third blend state on attachment 0 (one neither build above
//            used) and the recipe's formats, write masks and other attachments
// Both databases are opened read-only and immutable. Copy them first; never open them while GW2
// runs. No Wine, no game.
//
//   clang -x objective-c -std=gnu17 -fobjc-arc -O1 -Wall -arch x86_64 -mmacosx-version-min=26.0 \
//     tests/gw2/mtl4_specialize_test.m -framework Metal -framework Foundation -lsqlite3 -o $T/specialize
//   $T/specialize <recipes_1.db> <shaders_320.db> [first] [count]
//
// Metal caches compiled pipelines for command-line tools in
// $(getconf DARWIN_USER_CACHE_DIR)com.apple.metal/ (the perf review found its own runs in
// 16777235_355/ there). A cold run needs that folder emptied first, as stagecache's first run had it.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sqlite3.h>
#include <sys/qos.h>

static double
now_ms(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom)
    mach_timebase_info(&tb);
  return mach_absolute_time() * (double)tb.numer / tb.denom / 1e6;
}

// --- recipe rows (src/dxmt/dxmt_compile_recipe.cpp, serialize; schema 1)

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

typedef struct {
  MTLPixelFormat format;
  MTLBlendOperation rgb_op, alpha_op;
  MTLBlendFactor src_rgb, dst_rgb, src_alpha, dst_alpha;
  MTLColorWriteMask mask;
  int blending;
} Color;

typedef struct {
  NSData *vs_sha, *vs_var, *ps_sha, *ps_var;
  Color color[8];
  int alpha_to_coverage, logic, rasterization, samples;
  MTLPrimitiveTopologyClass topology;
} Recipe;

static int
parse(const void *blob, int length, Recipe *out) {
  Rd r = {blob, (const uint8_t *)blob + length, 1};
  if (u32(&r) != 1)
    return 0;
  u32(&r); // metal target
  out->vs_sha = dig(&r);
  out->vs_var = dig(&r);
  out->ps_sha = out->ps_var = nil;
  if (u8(&r)) {
    out->ps_sha = dig(&r);
    out->ps_var = dig(&r);
  }
  for (int i = 0; i < 8; i++) {
    Color *c = &out->color[i];
    c->format = (MTLPixelFormat)(u32(&r) & ~(0xfffu << 12));
    c->rgb_op = (MTLBlendOperation)u32(&r);
    c->alpha_op = (MTLBlendOperation)u32(&r);
    c->src_rgb = (MTLBlendFactor)u32(&r);
    c->dst_rgb = (MTLBlendFactor)u32(&r);
    c->src_alpha = (MTLBlendFactor)u32(&r);
    c->dst_alpha = (MTLBlendFactor)u32(&r);
    c->mask = (MTLColorWriteMask)u8(&r);
    c->blending = u8(&r);
  }
  out->alpha_to_coverage = u8(&r);
  out->logic = u8(&r);
  u32(&r); // logic operation
  out->rasterization = u8(&r);
  out->samples = u8(&r);
  u32(&r); // depth and stencil formats: an MTL4RenderPipelineDescriptor has neither
  u32(&r);
  u32(&r); // immutable vertex and fragment buffers: nor these
  u32(&r);
  out->topology = (MTLPrimitiveTopologyClass)u32(&r);
  u32(&r); // tessellation (unused by GW2's recipes) and ICB support
  u8(&r);
  u32(&r);
  u32(&r);
  u8(&r);
  return r.ok && r.p == r.end;
}

// --- descriptors

static void
set_color(MTL4RenderPipelineColorAttachmentDescriptor *d, const Color *c) {
  d.pixelFormat = c->format;
  d.writeMask = c->mask;
  d.blendingState = c->blending ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
  d.rgbBlendOperation = c->rgb_op;
  d.alphaBlendOperation = c->alpha_op;
  d.sourceRGBBlendFactor = c->src_rgb;
  d.destinationRGBBlendFactor = c->dst_rgb;
  d.sourceAlphaBlendFactor = c->src_alpha;
  d.destinationAlphaBlendFactor = c->dst_alpha;
}

static void
set_unspecialized(MTL4RenderPipelineColorAttachmentDescriptor *d) {
  d.pixelFormat = MTLPixelFormatUnspecialized;
  d.writeMask = MTLColorWriteMaskUnspecialized;
  d.blendingState = MTL4BlendStateUnspecialized;
  d.rgbBlendOperation = MTLBlendOperationUnspecialized;
  d.alphaBlendOperation = MTLBlendOperationUnspecialized;
  d.sourceRGBBlendFactor = MTLBlendFactorUnspecialized;
  d.destinationRGBBlendFactor = MTLBlendFactorUnspecialized;
  d.sourceAlphaBlendFactor = MTLBlendFactorUnspecialized;
  d.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
}

// colors: the 8 attachments' state, or NULL for the used ones (format != invalid) unspecialized
static MTL4RenderPipelineDescriptor *
describe(MTL4FunctionDescriptor *vs, MTL4FunctionDescriptor *ps, const Recipe *r, const Color *colors) {
  MTL4RenderPipelineDescriptor *d = [MTL4RenderPipelineDescriptor new];
  d.vertexFunctionDescriptor = vs;
  d.fragmentFunctionDescriptor = ps;
  for (int i = 0; i < 8; i++) {
    if (colors)
      set_color(d.colorAttachments[i], &colors[i]);
    else if (r->color[i].format != MTLPixelFormatInvalid)
      set_unspecialized(d.colorAttachments[i]);
  }
  d.alphaToCoverageState = r->alpha_to_coverage ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
  d.rasterizationEnabled = YES;
  d.rasterSampleCount = r->samples ? r->samples : 1;
  d.inputPrimitiveTopology = r->topology;
  return d;
}

static int
same_blend(const Color *a, const Color *b) {
  if (a->blending != b->blending || a->mask != b->mask)
    return 0;
  return !a->blending || (a->src_rgb == b->src_rgb && a->dst_rgb == b->dst_rgb && a->src_alpha == b->src_alpha &&
                          a->dst_alpha == b->dst_alpha && a->rgb_op == b->rgb_op && a->alpha_op == b->alpha_op);
}

// stagecache's "other blend" on attachment 0: additive if the recipe blends, alpha blending if not
static Color
other_blend(Color c) {
  int was = c.blending;
  c.blending = 1;
  c.rgb_op = c.alpha_op = MTLBlendOperationAdd;
  c.src_rgb = was ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;
  c.dst_rgb = was ? MTLBlendFactorOne : MTLBlendFactorOneMinusSourceAlpha;
  c.src_alpha = MTLBlendFactorOne;
  c.dst_alpha = was ? MTLBlendFactorOne : MTLBlendFactorOneMinusSourceAlpha;
  c.mask = was ? MTLColorWriteMaskAll : (MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue);
  return c;
}

// the specialization's attachment 0: the first of these that neither the recipe nor `other` uses
static Color
third_blend(Color c, const Color *recipe, const Color *other) {
  const MTLBlendFactor kSrc[3] = {MTLBlendFactorOne, MTLBlendFactorDestinationColor, MTLBlendFactorZero};
  const MTLBlendFactor kDst[3] = {MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorZero, MTLBlendFactorSourceColor};
  for (int i = 0; i < 3; i++) {
    c.blending = 1;
    c.rgb_op = c.alpha_op = MTLBlendOperationAdd;
    c.src_rgb = c.src_alpha = kSrc[i];
    c.dst_rgb = c.dst_alpha = kDst[i];
    c.mask = MTLColorWriteMaskAll;
    if (!same_blend(&c, recipe) && !same_blend(&c, other))
      break;
  }
  return c;
}

// --- the test (no arguments)

static const char *kSource = "#include <metal_stdlib>\n"
                             "using namespace metal;\n"
                             "struct V { float4 position [[position]]; float4 color; };\n"
                             "vertex V vs_main(uint id [[vertex_id]]) {\n"
                             "  V v; v.position = float4(float(id & 1), float(id >> 1), 0, 1);\n"
                             "  v.color = float4(1, 0.5, 0.25, 0.5); return v;\n"
                             "}\n"
                             "struct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\n"
                             "fragment O fs_main(V in [[stage_in]]) { O o; o.c0 = in.color; o.c1 = in.color.wzyx; return o; }\n";

static int
self_test(id<MTLDevice> device, id<MTL4Compiler> compiler) {
  NSError *err = nil;
  id<MTLLibrary> lib = [device newLibraryWithSource:@(kSource) options:nil error:&err];
  if (!lib) {
    printf("FAIL: library: %s\n", err.description.UTF8String);
    return 1;
  }
  MTL4LibraryFunctionDescriptor *vs = [MTL4LibraryFunctionDescriptor new], *ps = [MTL4LibraryFunctionDescriptor new];
  vs.name = @"vs_main";
  vs.library = lib;
  ps.name = @"fs_main";
  ps.library = lib;
  Recipe r = {0};
  r.topology = MTLPrimitiveTopologyClassTriangle;
  r.samples = 1;
  Color colors[8] = {{0}};
  colors[0] = (Color){MTLPixelFormatRGBA8Unorm, MTLBlendOperationAdd, MTLBlendOperationAdd, MTLBlendFactorOne, MTLBlendFactorZero,
                      MTLBlendFactorOne, MTLBlendFactorZero, MTLColorWriteMaskAll, 0};
  colors[1] = colors[0];
  colors[1].format = MTLPixelFormatRGBA16Float;
  r.color[0] = colors[0];
  r.color[1] = colors[1];
  int failures = 0;
  id<MTLRenderPipelineState> unspec = [compiler newRenderPipelineStateWithDescriptor:describe(vs, ps, &r, NULL)
                                                                  compilerTaskOptions:nil
                                                                                error:&err];
  if (!unspec) {
    printf("FAIL: unspecialized pipeline: %s\n", err.description.UTF8String);
    return 1;
  }
  Color other = other_blend(colors[0]);
  colors[0] = third_blend(colors[0], &r.color[0], &other);
  err = nil;
  id<MTLRenderPipelineState> spec = [compiler newRenderPipelineStateBySpecializationWithDescriptor:describe(vs, ps, &r, colors)
                                                                                           pipeline:unspec
                                                                                              error:&err];
  if (!spec) {
    printf("FAIL: specialization: %s\n", err.description.UTF8String);
    failures++;
  }
  if (same_blend(&colors[0], &r.color[0]) || same_blend(&colors[0], &other)) {
    printf("FAIL: the third blend state repeats one of the other two\n");
    failures++;
  }
  printf(failures ? "FAILED: %d\n" : "OK: an unspecialized pipeline and its specialization built\n", failures);
  return failures ? 1 : 0;
}

// --- the benchmark

static sqlite3 *
open_ro(const char *path) {
  sqlite3 *db = NULL;
  NSString *uri = [NSString
      stringWithFormat:@"file:%@?mode=ro&immutable=1",
                       [[NSString stringWithUTF8String:path]
                           stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet]];
  if (sqlite3_open_v2(uri.UTF8String, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
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

static void
report(const char *name, double *t, int n) {
  if (!n)
    return;
  qsort(t, n, sizeof(double), cmpd);
  double sum = 0;
  for (int i = 0; i < n; i++)
    sum += t[i];
  printf("%-7s %6d %8.2f %8.2f %8.2f %8.2f\n", name, n, sum / n, t[n / 2], t[(int)(n * 0.9)], t[n - 1]);
}

enum { FULL, OTHER, UNSPEC, SPEC, NSTAGE };

static int
bench(id<MTLDevice> device, id<MTL4Compiler> compiler, const char *recipes, const char *shaders, int first, int count) {
  sqlite3 *rdb = open_ro(recipes), *adb = open_ro(shaders);
  sqlite3_stmt *all, *get;
  if (sqlite3_prepare_v2(rdb, "SELECT value FROM cache_1 WHERE substr(key,1,1) = x'52' ORDER BY rowid;", -1, &all, NULL) !=
          SQLITE_OK ||
      sqlite3_prepare_v2(adb, "SELECT value FROM cache_26 WHERE key = ?;", -1, &get, NULL) != SQLITE_OK) {
    fprintf(stderr, "prepare: %s / %s\n", sqlite3_errmsg(rdb), sqlite3_errmsg(adb));
    return 1;
  }
  NSMutableDictionary<NSData *, id<MTLLibrary>> *libraries = [NSMutableDictionary dictionary];
  const char *names[NSTAGE] = {"full", "other", "unspec", "spec"};
  double *all_t[NSTAGE], *real_t[NSTAGE]; // every measured row; rows whose full build took over 3 ms
  for (int s = 0; s < NSTAGE; s++) {
    all_t[s] = calloc(count, sizeof(double));
    real_t[s] = calloc(count, sizeof(double));
  }
  int row = 0, measured = 0, real = 0, skipped = 0, noair = 0, failed = 0, bad = 0;
  char first_error[512] = "";
  double t_start = now_ms();

  while (sqlite3_step(all) == SQLITE_ROW && measured < count) {
    if (row++ < first)
      continue;
    @autoreleasepool {
      Recipe r;
      if (!parse(sqlite3_column_blob(all, 0), sqlite3_column_bytes(all, 0), &r) || r.logic) {
        bad++;
        continue;
      }
      if (!r.ps_sha || !r.rasterization || r.color[0].format == MTLPixelFormatInvalid) {
        skipped++;
        continue;
      }
      // the functions, as DXMT names them (CreateVariantShader): "vs_"/"ps_" + 8 hex of the SHA-1 + "_" + digest
      MTL4LibraryFunctionDescriptor *fd[2];
      int miss = 0;
      for (int s = 0; s < 2 && !miss; s++) {
        NSMutableData *key = [NSMutableData dataWithData:s ? r.ps_sha : r.vs_sha];
        [key appendData:s ? r.ps_var : r.vs_var];
        id<MTLLibrary> lib = libraries[key];
        if (!lib) {
          sqlite3_reset(get);
          sqlite3_bind_blob(get, 1, key.bytes, (int)key.length, SQLITE_STATIC);
          if (sqlite3_step(get) != SQLITE_ROW) {
            miss = 1;
            break;
          }
          dispatch_data_t data =
              dispatch_data_create(sqlite3_column_blob(get, 0), sqlite3_column_bytes(get, 0), nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
          NSError *err = nil;
          lib = [device newLibraryWithData:data error:&err];
          if (!lib) {
            miss = 1;
            break;
          }
          libraries[key] = lib;
        }
        fd[s] = [MTL4LibraryFunctionDescriptor new];
        fd[s].library = lib;
        fd[s].name = [NSString stringWithFormat:@"%s_%@_%@", s ? "ps" : "vs", [hex(s ? r.ps_sha : r.vs_sha) substringToIndex:8],
                                                hex(s ? r.ps_var : r.vs_var)];
      }
      if (miss) {
        noair++;
        continue;
      }
      Color other[8], third[8];
      memcpy(other, r.color, sizeof(other));
      memcpy(third, r.color, sizeof(third));
      other[0] = other_blend(r.color[0]);
      third[0] = third_blend(r.color[0], &r.color[0], &other[0]);

      double ms[NSTAGE];
      id<MTLRenderPipelineState> pso[NSTAGE] = {nil};
      NSError *err = nil;
      for (int s = 0; s < NSTAGE && (s == 0 || pso[s - 1]); s++) {
        err = nil;
        double t0 = now_ms();
        switch (s) {
        case FULL:
          pso[s] = [compiler newRenderPipelineStateWithDescriptor:describe(fd[0], fd[1], &r, r.color) compilerTaskOptions:nil error:&err];
          break;
        case OTHER:
          pso[s] = [compiler newRenderPipelineStateWithDescriptor:describe(fd[0], fd[1], &r, other) compilerTaskOptions:nil error:&err];
          break;
        case UNSPEC:
          pso[s] = [compiler newRenderPipelineStateWithDescriptor:describe(fd[0], fd[1], &r, NULL) compilerTaskOptions:nil error:&err];
          break;
        case SPEC:
          pso[s] = [compiler newRenderPipelineStateBySpecializationWithDescriptor:describe(fd[0], fd[1], &r, third)
                                                                         pipeline:pso[UNSPEC]
                                                                            error:&err];
          break;
        }
        ms[s] = now_ms() - t0;
        if (!pso[s] && !first_error[0])
          snprintf(first_error, sizeof(first_error), "row %d, %s: %s", row - 1, names[s],
                   err ? err.localizedDescription.UTF8String : "no error");
      }
      if (!pso[SPEC]) {
        failed++;
        continue;
      }
      for (int s = 0; s < NSTAGE; s++)
        all_t[s][measured] = ms[s];
      measured++;
      if (ms[FULL] > 3) { // as stagecache: only where the first build was a real compile
        for (int s = 0; s < NSTAGE; s++)
          real_t[s][real] = ms[s];
        real++;
      }
    }
  }
  printf("first=%d measured=%d skipped=%d (no pixel shader, rasterization off or no attachment 0) noair=%d failed=%d bad=%d "
         "total=%.1f s\n",
         first, measured, skipped, noair, failed, bad, (now_ms() - t_start) / 1000);
  if (first_error[0])
    printf("first failure: %s\n", first_error);
  printf("\nevery measured recipe, ms:\n%-7s %6s %8s %8s %8s %8s\n", "stage", "n", "mean", "median", "p90", "max");
  for (int s = 0; s < NSTAGE; s++)
    report(names[s], all_t[s], measured);
  printf("\nwhere full took over 3 ms (stagecache's filter for its 8.7 ms), ms:\n%-7s %6s %8s %8s %8s %8s\n", "stage", "n",
         "mean", "median", "p90", "max");
  for (int s = 0; s < NSTAGE; s++)
    report(names[s], real_t[s], real);
  sqlite3_finalize(all);
  sqlite3_finalize(get);
  sqlite3_close(rdb);
  sqlite3_close(adb);
  return measured ? 0 : 1;
}

int
main(int argc, char **argv) {
  @autoreleasepool {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); // as stagecache's default
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || ![device supportsFamily:MTLGPUFamilyMetal4]) {
      printf("no Metal 4 device here: skipped\n");
      return 77; // X1: skipped, not passed
    }
    device.shouldMaximizeConcurrentCompilation = YES; // as DXMT sets it
    printf("device: %s\n", device.name.UTF8String);
    NSError *err = nil;
    id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    if (!compiler) {
      printf("FAIL: newCompilerWithDescriptor: %s\n", err.description.UTF8String);
      return 1;
    }
    if (argc < 3)
      return self_test(device, compiler);
    return bench(device, compiler, argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 1500);
  }
}
