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

#include "dxmt_gw2_thread_priority.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace dxmt::compile {
struct JobRecord; // dxmt_compile_service.hpp
}

namespace dxmt::gw2 {

// Render thread / any thread.
void compileVariantNew();
// gw2 (#160): a shader's first variant was just made; `created` is compileClock() when the shader was
// created (CreateShader, after SM50Initialize). Nothing without DXMT_STATS (created = 0).
void compileFirstVariant(int64_t created);
void compilePipelineLookup(bool hit);
// Compile workers. compileClock() is the start of a build (0, no clock read, without DXMT_STATS);
// the *Built calls take it. compilePipelineBuilt also counts for the archive flushes.
void compileAirLookup(bool hit);
int64_t compileClock();
void compileShaderBuilt(int64_t t0);
// gw2 (W0): one stage of a shader variant's build, from `t0` (compileClock()) to now; nothing without
// DXMT_STATS (t0 = 0). Lock: waiting for the AIR cache reader's lock; Air: the AIR read; Library:
// newLibrary; Function: newFunction; Translate: airconv, on an AIR miss; Store: writing the new AIR
// (the writer's lock included).
enum class VariantStage : uint32_t { Lock, Air, Library, Function, Translate, Store, Count };
void compileVariantStage(VariantStage stage, int64_t t0);
void compilePipelineBuilt(int64_t t0, bool ok);
// The compile scheduler: a task was submitted (+1) or completed (-1); its worker count.
void compileQueue(int64_t delta);
void compileThreads(uint64_t threads);
// gw2 (#159): the speculative jobs running now, at each change (the WARM section's jobs=; it is defined
// there, in dxmt_gw2_warm.cpp)
void warmJobsRunning(uint32_t running);

// The encode thread (or whichever thread encodes chunks), before encoding a chunk of `frame`.
void compileEncodeFrame(uint64_t frame);

// gw2 (#46, C1): the compile jobs' timelines (compile::JobRecord, owned by the compile service) are
// stamped only with DXMT_STATS on; the service reads the clock through these.
bool compileTiming();   // DXMT_STATS on
int64_t compileNowNs(); // the timeline's clock

// gw2 (C2): DXMT_COMPILE_DEMAND=1 (off by default): the compile service's declared policy (dependencies
// taken at submit, a demand lane, speculative admission). Parsed once per process.
bool compileDemand();
// A demand promoted a job (counted for COMPILE's promoted=, with DXMT_STATS).
void compilePromoted();

// gw2 (#152): DXMT_COMPILE_PRIORITY, the compile workers' priority: critical (default), normal or
// utility, as DXMT_THREAD_PRIORITY's (dxmt_gw2_thread_priority.hpp). compilePriority() reads it, at
// each device's pipeline cache; each worker applies it at its start (compileWorkerPriority).
ThreadPriority compilePriority();
void compileWorkerPriority(ThreadPriority mode);

// Around a demand for a job that isn't done yet (compile::demand); with the job's record, the wait
// is classified by what the job was doing when it started (#46).
class CompileWaitScope {
public:
  explicit CompileWaitScope(const compile::JobRecord *job = nullptr);
  ~CompileWaitScope();
  CompileWaitScope(const CompileWaitScope &) = delete;
  CompileWaitScope &operator=(const CompileWaitScope &) = delete;

private:
  int64_t t0_ = 0;
  const compile::JobRecord *job_ = nullptr;
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
