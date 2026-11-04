// gw2-test: metal
// gw2-build: F=(-x objective-c -std=gnu17 -fblocks -fno-objc-arc -O1 -DPSO_ARCHIVE_TESTING -mmacosx-version-min=13.0 -Isrc/winemetal -Isrc/winemetal/unix -Isrc/airconv)
// gw2-build: for arch in arm64 x86_64; do
// gw2-build:   clang $F -arch $arch src/winemetal/unix/pso_archive.c src/winemetal/unix/pso_archive_policy.c src/winemetal/unix/pso_archive_publish.c tests/gw2/pso_archive_test.m -framework Metal -framework Foundation -o ${OUT}_new_$arch
// gw2-build:   clang $F -arch $arch -DPSO_SESSION_LEGACY tests/gw2/fixtures/legacy_pso_archive.c tests/gw2/pso_archive_test.m -framework Metal -framework Foundation -o ${OUT}_legacy_$arch
// gw2-build: done
// gw2-run: for arch in arm64 x86_64; do
// gw2-run:   tests/gw2/pso_archive_parity.zsh ${OUT}_legacy_$arch ${OUT}_new_$arch "$T/pso_archive_parity_$arch"
// gw2-run:   tests/gw2/pso_archive_generations.zsh ${OUT}_new_$arch "$T/pso_archive_generations_$arch"
// gw2-run: done
//
// gw2 (#75): one PSO archive "session" (one process), driven through the bridge's unix entry points
// (_WMTPSOArchiveInit/_Flush/_Query, pso_archive_new_render/_compute) as the game drives them. Built
// against src/winemetal/unix/pso_archive.c and against tests/gw2/fixtures/legacy_pso_archive.c (#55),
// both with -DPSO_ARCHIVE_TESTING, for arm64 and for x86_64 (the game's architecture, under Rosetta).
// The scenarios are in tests/gw2/pso_archive_parity.zsh (legacy vs the instance with the generation
// publisher off: same reports, counters, files and keys) and tests/gw2/pso_archive_generations.zsh
// (the publisher). The driver alone:
//
//   pso_archive_test_new_arm64 <base dir> <script...>
//
// Script words, in order:
//   b<i>[-<j>]  build pipelines i..j (a fixed table of shapes: see build())
//   d<i>        build pipeline i again (a second pipeline with the same key)
//   f           flush; prints "flush ret=N" and the report
//   q           prints the stats (every field)
//   s<n>        sleeps n seconds
// Environment: PSO_SESSION_KEEP (sessions kept, default 4), PSO_SESSION_MAX (pipelines per session,
// default 8192), PSO_SESSION_GENERATIONS=1 (the generation publisher; the legacy build ignores it),
// PSO_ARCHIVE_TEST_FAIL/_INDEX (fake packer failures, pso_archive.c).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define WINEMETAL_API
#include "winemetal_thunks.h"
#include "pso_archive.h"

enum { kShapes = 64 };

static NSString *
source(void) {
  NSMutableString *s = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\n"];
  for (int i = 0; i < kShapes; i++) {
    [s appendFormat:@"vertex float4 vs_%d(uint id [[vertex_id]]) { return float4(%d.0, float(id), 0.0, 1.0); }\n", i, i];
    [s appendFormat:@"kernel void cs_%d(device float *b [[buffer(0)]], uint id [[thread_position_in_grid]]) "
                    @"{ b[id] = %d.0; }\n", i, i];
  }
  [s appendString:@"fragment float4 fs_0(float4 p [[position]]) { return float4(p.x, 0.0, 0.0, 1.0); }\n"];
  return s;
}

static id<MTLLibrary> g_lib;

// Shape i: i % 8 == 5 compute (cs_i); i % 4 == 3 fragment-less, depth only (vs_i); else render
// vs_i + fs_0 into one color attachment (RGBA8 or BGRA8, blending on for odd i).
static id
build(id<MTLDevice> dev, int i, NSError **err) {
  if (i % 8 == 5) {
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = [[g_lib newFunctionWithName:[NSString stringWithFormat:@"cs_%d", i]] autorelease];
    id pso = pso_archive_new_compute(dev, d, err);
    [d release];
    return pso;
  }
  MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
  d.vertexFunction = [[g_lib newFunctionWithName:[NSString stringWithFormat:@"vs_%d", i]] autorelease];
  if (i % 4 == 3) {
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  } else {
    d.fragmentFunction = [[g_lib newFunctionWithName:@"fs_0"] autorelease];
    d.colorAttachments[0].pixelFormat = (i / 2) % 2 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
    d.colorAttachments[0].blendingEnabled = i % 2;
  }
  id pso = pso_archive_new_render(dev, d, err);
  [d release];
  return pso;
}

static void
print_stats(void) {
  struct WMTPSOArchiveStats s;
  memset(&s, 0, sizeof(s));
  struct unixcall_wmt_pso_archive_query qp = {};
  qp.stats.ptr = &s;
  _WMTPSOArchiveQuery(&qp);
  printf("stats loaded=%llu hits=%llu misses=%llu added=%llu add_failed=%llu capped=%llu queued=%llu "
         "unflushed=%llu flushes=%llu miss_known=%llu miss_new=%llu skipped=%llu rebuilds=%llu\n",
         s.loaded, s.lookup_hits, s.lookup_misses, s.added, s.add_failed, s.capped, s.queued, s.unflushed, s.flushes,
         s.miss_known, s.miss_new, s.skipped, s.rebuilds);
  if (getenv("PSO_SESSION_GENERATIONS"))
    printf("gen mode=%llu miss_excluded=%llu excluded=%llu generation=%llu published=%llu\n", s.mode, s.miss_excluded,
           s.excluded, s.generation, s.published);
}

int
main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 2)
      return 2;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev)
      return 77;
    NSError *err = nil;
    g_lib = [dev newLibraryWithSource:source() options:nil error:&err];
    if (!g_lib) {
      fprintf(stderr, "library: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    struct WMTPSOArchiveConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.exe_name.ptr = "session.exe";
    cfg.base_dir.ptr = argv[1];
    cfg.keep_sessions = getenv("PSO_SESSION_KEEP") ? (uint32_t)atoi(getenv("PSO_SESSION_KEEP")) : 4;
    cfg.max_pipelines = getenv("PSO_SESSION_MAX") ? (uint32_t)atoi(getenv("PSO_SESSION_MAX")) : 8192;
#ifndef PSO_SESSION_LEGACY
    cfg.generations = getenv("PSO_SESSION_GENERATIONS") ? 1 : 0;
#endif
    char report[16384] = "";
    struct unixcall_wmt_pso_archive_init ip = {};
    ip.device = (obj_handle_t)dev;
    ip.config.ptr = &cfg;
    ip.report.ptr = report;
    ip.report_size = sizeof(report);
    _WMTPSOArchiveInit(&ip);
    printf("init ret=%llu\n%s", (unsigned long long)ip.ret, report);
    for (int w = 2; w < argc; w++) {
      const char *word = argv[w];
      if (word[0] == 'b') {
        int from = atoi(word + 1), to = from;
        const char *dash = strchr(word, '-');
        if (dash)
          to = atoi(dash + 1);
        for (int i = from; i <= to; i++)
          if (!build(dev, i % kShapes, &err))
            printf("build %d failed: %s\n", i, err.localizedDescription.UTF8String);
      } else if (word[0] == 'd') {
        if (!build(dev, atoi(word + 1) % kShapes, &err))
          printf("build %s failed\n", word);
      } else if (word[0] == 'f') {
        char rep[16384] = "";
        struct unixcall_wmt_pso_archive_flush fp = {};
        fp.report.ptr = rep;
        fp.report_size = sizeof(rep);
        _WMTPSOArchiveFlush(&fp);
        printf("flush ret=%lld\n%s", (long long)fp.ret, rep);
      } else if (word[0] == 'q') {
        print_stats();
      } else if (word[0] == 's') {
        sleep((unsigned)atoi(word + 1));
      }
    }
    fflush(stdout);
  }
  return 0;
}
