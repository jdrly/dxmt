#pragma once

// gw2 (#5): shader and pipeline compile telemetry, and the PE side of the PSO archives.
//
// - The COMPILE section (2 s window, DXMT_STATS): new shader variants, AIR cache hits/misses,
//   pipeline cache hits/misses, pipelines built, build time, the compile queue, and the encode
//   thread's waits for pipelines that weren't ready (GetPipeline).
// - A per-frame "COMPILE frame=..." line for a frame whose encode-thread waits reach
//   DXMT_COMPILE_WAIT_MS (default 2), at most one per 200 ms, with the variants, cache lookups
//   and builds of all threads while that frame was encoded.
// - DXMT_PSO_ARCHIVE=1 (off by default): Metal binary archives for pipelines, kept on the unix
//   side of the bridge (src/winemetal/unix/pso_archive.c); a thread here serializes them every
//   DXMT_PSO_FLUSH_EVERY new pipelines and logs each flush.
//
// Everything is counted only with DXMT_STATS on, except what the archive needs.

#include <cstdint>

namespace dxmt::gw2 {

// Render thread / any thread.
void compileVariantNew();
void compilePipelineLookup(bool hit);
// Compile workers. compileClock() is the start of a build (0, no clock read, without DXMT_STATS);
// the *Built calls take it. compilePipelineBuilt also counts for the archive flushes.
void compileAirLookup(bool hit);
int64_t compileClock();
void compileShaderBuilt(int64_t t0);
void compilePipelineBuilt(int64_t t0, bool ok);
// The compile scheduler: a task was submitted (+1) or completed (-1); its worker count.
void compileQueue(int64_t delta);
void compileThreads(uint64_t threads);

// The encode thread (or whichever thread encodes chunks), before encoding a chunk of `frame`.
void compileEncodeFrame(uint64_t frame);

// Around a wait in GetPipeline for a pipeline that isn't ready yet.
class CompileWaitScope {
public:
  CompileWaitScope();
  ~CompileWaitScope();
  CompileWaitScope(const CompileWaitScope &) = delete;
  CompileWaitScope &operator=(const CompileWaitScope &) = delete;

private:
  int64_t t0_ = 0;
};

// PSO archives: once per process, with the Metal device (no-op unless DXMT_PSO_ARCHIVE=1).
void psoArchiveInit(uint64_t mtl_device);
// Asks for a flush (non-blocking), e.g. when a device goes away.
void psoArchiveRequestFlush();

} // namespace dxmt::gw2
