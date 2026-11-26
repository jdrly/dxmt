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

#include <atomic>
#include <cstdint>
#include <string>

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

// gw2 (#46): a compile task's timeline, stamped by the scheduler (submit, run, parked, done) and by
// the pipeline tasks (build), with DXMT_STATS only; the encode thread reads it when it has to wait
// for the task. A task submitted while the stats were off stays `None` and isn't classified.
struct CompileTrace {
  enum State : uint8_t { None = 0, Queued, Running, Parked, Building, Done };
  std::atomic<uint8_t> state{None};
  std::atomic<int64_t> submit_ns{0}, start_ns{0}, build_ns{0}, done_ns{0}, parked_at_ns{0}, parked_ns{0};
  std::atomic<const CompileTrace *> dep{nullptr}; // what it was last parked on (a shader variant)
};
void compileTraceSubmit(CompileTrace &t);
void compileTraceRun(CompileTrace &t);                           // a worker starts or resumes it
void compileTraceParked(CompileTrace &t, const CompileTrace *dep); // it returned a dependency
void compileTraceBuild(CompileTrace &t);                         // the Metal pipeline build starts
void compileTraceDone(CompileTrace &t);

// Around a wait in GetPipeline for a pipeline that isn't ready yet; with the pipeline's trace, the
// wait is classified by what the pipeline was doing when it started (#46).
class CompileWaitScope {
public:
  explicit CompileWaitScope(const CompileTrace *trace = nullptr);
  ~CompileWaitScope();
  CompileWaitScope(const CompileWaitScope &) = delete;
  CompileWaitScope &operator=(const CompileWaitScope &) = delete;

private:
  int64_t t0_ = 0;
  const CompileTrace *trace_ = nullptr;
  int cause_ = 0;
  int64_t depth_ = 0;
  int64_t archq_ = -1;
};

// The switches as parsed (once), for the code and the startup line.
struct CompileSettings {
  double wait_ms = 2;             // DXMT_COMPILE_WAIT_MS
  bool archive_requested = false; // DXMT_PSO_ARCHIVE=1
  std::string archive_dir;        // DXMT_PSO_ARCHIVE_DIR; empty: the user cache dir
  uint64_t flush_every = 256;     // DXMT_PSO_FLUSH_EVERY
};
const CompileSettings &compileSettings();
// The archives are on: requested, and psoArchiveInit got them started.
bool psoArchiveOn();

// PSO archives: once per process, with the Metal device (no-op unless DXMT_PSO_ARCHIVE=1).
void psoArchiveInit(uint64_t mtl_device);
// Asks for a flush (non-blocking), e.g. when a device goes away.
void psoArchiveRequestFlush();

} // namespace dxmt::gw2
