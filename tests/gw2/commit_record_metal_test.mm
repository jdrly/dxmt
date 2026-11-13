// gw2-test: metal
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   clang++ -std=c++20 -fobjc-arc -Wall -Wextra -arch $arch -mmacosx-version-min=26.0 -Isrc/dxmt tests/gw2/commit_record_metal_test.mm -framework Metal -framework Foundation -o ${OUT}_$arch
// gw2-build: done
// gw2-run: ${OUT}_x86_64 && MTL_DEBUG_LAYER=1 ${OUT}_arm64
//
// gw2 (#77, Q1, architecture council §2): the Metal 3 commit records
// (src/dxmt/dxmt_gw2_commit_record.hpp) on real command buffers. The production template
// (recordMetal3Commit, reportCompletedChunk) reads real MTLCommandBuffers here, through a wrapper that
// reads them as the bridge's unix side does (winemetal_unix.c: status, waitUntilCompleted, error, logs,
// GPUStartTime/GPUEndTime × 1e9). No Wine, no bridge, no game. The Metal 4 conversion is checked by
// tests/gw2/commit_record_test.cpp, and Metal 4's feedback times by tests/gw2/mtl4_queue_test.mm.
//
// What it checks, for chunks shaped as DXMT commits them on Metal 3 (the split buffers first, then the
// last one, all on one queue; the records built at once, so the buffers are still pending):
// - an empty chunk (one command buffer with no encoder), and 0, 1 and 3 split buffers with GPU work;
// - every record is in commit order (ordinals 0..n-1), and every buffer completed without an error
//   once the records are built (the builder waited for each);
// - with timing: GPU times for every buffer with work (end >= start, each record its own buffer's);
//   without: none read, and no logs for a split buffer without an error;
// - the report: one GPU span per record and the last one's end as aftergpu, with DXMT_STATS only;
//   no device-error line.
// No deliberate GPU faults (they aren't deterministic: council §2). Exit 77 without a Metal device.

#import <Metal/Metal.h>
#include "dxmt_gw2_commit_record.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace dxmt::gw2;

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

struct MtlError {
  NSError *error = nil;
  explicit
  operator bool() const {
    return error != nil;
  }
};
using Record = BasicCommitRecord<MtlError>;

// a command buffer as the bridge reads it (winemetal_unix.c), for recordMetal3Commit
struct MtlBuffer {
  static constexpr auto kScheduled = MTLCommandBufferStatusScheduled;
  static constexpr auto kError = MTLCommandBufferStatusError;
  id<MTLCommandBuffer> cmdbuf;
  int waits = 0;
  int time_reads = 0;

  MTLCommandBufferStatus
  status() {
    return cmdbuf.status;
  }
  void
  waitUntilCompleted() {
    waits++;
    [cmdbuf waitUntilCompleted];
  }
  MtlError
  error() {
    return {cmdbuf.error};
  }
  void
  logs(std::vector<std::string> &out) {
    for (id<MTLFunctionLog> log in cmdbuf.logs)
      out.push_back(log.description.UTF8String);
  }
  uint64_t
  gpuStartTime() {
    time_reads++;
    return uint64_t(cmdbuf.GPUStartTime * 1000000000.0);
  }
  uint64_t
  gpuEndTime() {
    time_reads++;
    return uint64_t(cmdbuf.GPUEndTime * 1000000000.0);
  }
};

struct Sink {
  int lines = 0;
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  std::vector<uint64_t> aftergpu;

  std::chrono::steady_clock::time_point
  now() {
    return std::chrono::steady_clock::now();
  }
  void
  deviceError(const Record &) {
    lines++;
  }
  void
  lastLog(const Record &, const std::string &) {
    lines++;
  }
  void
  splitError(const Record &, size_t, size_t, uint64_t, uint64_t) {
    lines++;
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

// one command buffer of a chunk: with `work`, a blit that fills its 4 bytes of `buffer`
static id<MTLCommandBuffer>
commitBuffer(id<MTLCommandQueue> queue, id<MTLBuffer> buffer, uint32_t index, bool work) {
  id<MTLCommandBuffer> cmdbuf = [queue commandBuffer];
  if (work) {
    id<MTLBlitCommandEncoder> blit = [cmdbuf blitCommandEncoder];
    [blit fillBuffer:buffer range:NSMakeRange(index * 4, 4) value:uint8_t(index + 1)];
    [blit endEncoding];
  }
  [cmdbuf commit];
  return cmdbuf;
}

static void
chunk(id<MTLCommandQueue> queue, id<MTLBuffer> buffer, const char *name, int splits, bool work, bool timing) {
  // as the present split commits them: the split buffers, then the last one
  std::vector<MtlBuffer> buffers;
  for (int i = 0; i <= splits; i++)
    buffers.push_back({commitBuffer(queue, buffer, i, work)});

  // the finish thread (FinishChunkMetal3): the last buffer's record first, then the split buffers'
  std::vector<Record> records(buffers.size());
  recordMetal3Commit(buffers.back(), uint32_t(splits), true, timing, records.back());
  for (int i = 0; i < splits; i++)
    recordMetal3Commit(buffers[i], uint32_t(i), false, timing, records[i]);

  int waits = 0, time_reads = 0;
  for (size_t i = 0; i < buffers.size(); i++) {
    waits += buffers[i].waits;
    time_reads += buffers[i].time_reads;
    CHECK(records[i].ordinal == i, "%s: record %zu has ordinal %u", name, i, records[i].ordinal);
    CHECK(buffers[i].cmdbuf.status == MTLCommandBufferStatusCompleted, "%s: buffer %zu not completed (%ld)", name, i,
          long(buffers[i].cmdbuf.status));
    CHECK(!records[i].error, "%s: buffer %zu failed: %s", name, i, records[i].error.error.description.UTF8String);
    CHECK(records[i].logs.empty(), "%s: buffer %zu has logs", name, i);
    if (!timing) {
      CHECK(!records[i].gpu_start_ns && !records[i].gpu_end_ns, "%s: times without timing", name);
    } else if (work) {
      CHECK(records[i].gpu_start_ns && records[i].gpu_end_ns && *records[i].gpu_end_ns >= *records[i].gpu_start_ns,
            "%s: buffer %zu: no GPU span (%llu, %llu)", name, i,
            (unsigned long long)records[i].gpu_start_ns.value_or(0), (unsigned long long)records[i].gpu_end_ns.value_or(0));
      // gw2 (#257): each record has its own buffer's times. One queue runs its buffers in commit order, but
      // Metal doesn't promise that their GPUStartTimes rise in that order: the GitHub runner's paravirtual GPU
      // gave buffer 2 of "3 splits" an earlier start than buffer 1.
      CHECK(records[i].gpu_start_ns == uint64_t(buffers[i].cmdbuf.GPUStartTime * 1000000000.0) &&
                records[i].gpu_end_ns == uint64_t(buffers[i].cmdbuf.GPUEndTime * 1000000000.0),
            "%s: record %zu doesn't have buffer %zu's times", name, i, i);
    }
  }
  CHECK(timing ? time_reads == 2 * int(buffers.size()) : time_reads == 0, "%s: %d GPU time reads", name, time_reads);

  BasicSplitErrorThrottle<std::chrono::steady_clock::time_point> throttle;
  Sink sink;
  reportCompletedChunk(std::span<const Record>(records), ReportOrder::LastFirst, timing, throttle, sink);
  CHECK(sink.lines == 0, "%s: %d device-error lines", name, sink.lines);
  if (timing) {
    CHECK(sink.spans.size() == records.size(), "%s: %zu spans for %zu records", name, sink.spans.size(), records.size());
    CHECK(sink.aftergpu.size() == 1 && sink.aftergpu[0] == records.back().gpu_end_ns.value_or(0),
          "%s: aftergpu is the last buffer's end", name);
  } else {
    CHECK(sink.spans.empty() && sink.aftergpu.empty(), "%s: values without DXMT_STATS", name);
  }
  std::printf("%s %s: %zu records, %d waited for, times %s (last: %s)\n", failures ? "...." : "ok  ", name,
              records.size(), waits, timing ? "read" : "not read",
              !timing                          ? "-"
              : records.back().gpu_start_ns    ? "known"
                                               : "unknown, reported as 0");
}

int
main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::printf("SKIP: no Metal device\n");
      return 77;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];

    chunk(queue, buffer, "empty chunk", 0, false, true);
    chunk(queue, buffer, "0 splits", 0, true, true);
    chunk(queue, buffer, "1 split", 1, true, true);
    chunk(queue, buffer, "3 splits", 3, true, true);
    chunk(queue, buffer, "3 splits, no timing", 3, true, false);
    auto *bytes = (const uint8_t *)buffer.contents;
    CHECK(bytes[0] == 1 && bytes[4] == 2 && bytes[12] == 4, "the GPU did the chunks' work");
  }
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("commit_record_metal_test: all passed\n");
  return 0;
}
