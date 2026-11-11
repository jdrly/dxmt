/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Copyright 2026 Marc-Aurel Zent for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxmt_command_queue.hpp"
#include "dxmt_gw2_compile.hpp"
#include "dxmt_gw2_deps.hpp"
#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_residency.hpp"
#include "dxmt_gw2_metal4.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include "dxmt_gw2_thread_priority.hpp"
#include "Metal.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>
#include <utility>

#define ASYNC_ENCODING 1

namespace dxmt {

namespace {

// gw2 (#8): DXMT_CHUNKS, the ring's size: 32 (default, upstream's), 64 or 128 (strtoul's value;
// anything else is the default, with a warning). Read at each queue's creation, as before.
// gw2 (#60): declared here; the startup line shows the ring in effect (LogStartup's field).
sw::Switch<uint32_t> kChunks{"DXMT_CHUNKS", kCommandChunkCount, sw::legacy::ulongOneOf<32, 64, 128>};

// gw2 (#113): DXMT_RETIRE_RECOVERY (on; 0: off): a refused retirement (never expected) recovers by
// sequence with the completion its token proves, and refused holds go once their last chunk completed,
// instead of every later hold being kept until teardown. Read at each queue's creation.
sw::Switch<bool> kRetireRecovery{"DXMT_RETIRE_RECOVERY", true, sw::legacy::notZero};

uint32_t
ChunkCountFromEnv() {
  auto v = kChunks.requested();
  if (!v.empty() && !sw::legacy::ulongIsOneOf<32, 64, 128>(v))
    WARN("DXMT_CHUNKS=", v, " is not 32, 64 or 128; using ", kCommandChunkCount);
  return kChunks.parse(v);
}

// gw2 (#8): the Metal queue's bound on uncompleted command buffers. Unset DXMT_CHUNKS keeps
// upstream's 32 (the ring's size). Set, it is the ring plus headroom for the present split's extra
// buffer: one per frame in flight, and SetMaximumFrameLatency allows at most 16 frames.
uint32_t
MetalBufferCount(uint32_t chunks) {
  if (kChunks.requested().empty())
    return kCommandChunkCount;
  return chunks + 16;
}

// gw2 (#8): DXMT_MAX_FRAME_LATENCY, 1-16, replaces the latency the game sets; 0: not set.
uint32_t
MaxLatencyOverrideFromEnv() {
  auto v = env::getEnvVar("DXMT_MAX_FRAME_LATENCY");
  if (v.empty())
    return 0;
  auto n = std::strtoul(v.c_str(), nullptr, 10);
  if (n >= 1 && n <= 16)
    return n;
  WARN("DXMT_MAX_FRAME_LATENCY=", v, " is not 1-16; ignored");
  return 0;
}

// gw2 (#151): DXMT_THREAD_PRIORITY, the encode and finish threads' priority (critical, normal or
// utility; see dxmt_gw2_thread_priority.hpp). Read at each queue's creation.
gw2::ThreadPrioritySwitch kThreadPriority{"DXMT_THREAD_PRIORITY"};

// gw2 (#8): the RING section. The render thread publishes each completed frame's totals (at the
// next frame's first commit) under the mutex, and the 2 s window drains them under it, so a line
// only ever holds whole frames and its averages divide by the frames it holds.
struct RingWindow {
  uint64_t frames = 0;
  uint64_t commits = 0;
  uint64_t occupancy_sum = 0; // chunks in flight after each commit
  uint64_t peak = 0;
  uint64_t stalls = 0;
  uint64_t stalled_frames = 0;
  uint64_t stall_ns = 0;
  uint64_t frame_stall_max_ns = 0;
};
std::mutex g_ring_mutex;
RingWindow g_ring_window;
std::atomic<uint32_t> g_ring_chunks{kCommandChunkCount};

void
PublishRingFrame(const CommandQueue::RingFrame &f) {
  std::lock_guard lock(g_ring_mutex);
  auto &w = g_ring_window;
  w.frames++;
  w.commits += f.commits;
  w.occupancy_sum += f.occupancy_sum;
  w.peak = std::max(w.peak, f.peak);
  w.stalls += f.stalls;
  w.stalled_frames += f.stalls ? 1 : 0;
  w.stall_ns += f.stall_ns;
  w.frame_stall_max_ns = std::max(w.frame_stall_max_ns, f.stall_ns);
}

std::string
RingSection(double, uint32_t) {
  RingWindow w;
  {
    std::lock_guard lock(g_ring_mutex);
    w = std::exchange(g_ring_window, {});
  }
  if (!w.commits)
    return {};
  double n = w.frames;
  return std::format(
      "chunks={} occupancy={:.1f}/{} stalls={:.2f} stalled={} ms={:.2f}/{:.1f}", g_ring_chunks.load(),
      double(w.occupancy_sum) / w.commits, w.peak, w.stalls / n, w.stalled_frames, w.stall_ns / 1e6 / n,
      w.frame_stall_max_ns / 1e6
  );
}

gw2::SectionRegistrar g_ring_section("RING", RingSection);

} // namespace

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue->AllocateCommandData(size, alignment);
}

CommandQueue::CommandQueue(WMT::Device device) :
    chunk_count_(ChunkCountFromEnv()),
    metal_buffer_count_(MetalBufferCount(chunk_count_)),
    max_latency_override_(MaxLatencyOverrideFromEnv()),
    unretained_(gw2::lifetime::unretained()),
    lifetime_id_(gw2::lifetime::attachQueue()), // gw2 (#31): logs the command buffer mode once per queue
    retire_recovery_(kRetireRecovery()),        // gw2 (#113)
    thread_priority_(kThreadPriority.read()),   // gw2 (#151)
    // gw2 (#69): Metal 3's policy until the constructor knows whether the queue is on Metal 4
    ledger_(chunk_count_, {.unretained = unretained_, .lifetime_check = gw2::lifetime::check()}),
    encodeThread([this]() { this->EncodingThread(); }),
    finishThread([this]() { this->WaitForFinishThread(); }),
    device(device),
    commandQueue(device.newCommandQueue(metal_buffer_count_)),
    shared_event_listener(SharedEventListener_create()),
    event_listener_thread([this]() { SharedEventListener_start(this->shared_event_listener); }),
    staging_allocator({
        device, WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                    WMTResourceStorageModeManaged, false
    }),
    copy_temp_allocator({device, WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}),
    argbuf_allocator({
        device,
        WMTResourceHazardTrackingModeUntracked | WMTResourceCPUCacheModeWriteCombined | WMTResourceStorageModeShared,
        true, true // gw2 (#153): its blocks are in the device's residency set on Metal 4
    }),
    cpu_command_allocator({}),
    reftracker_storage_allocator({}),
    cmd_library(device),
    argument_encoding_ctx(*this, device, cmd_library),
    initializer(device),
    counter_pool(device) {
  for (unsigned i = 0; i < kCommandChunkCountMax; i++) {
    auto &chunk = chunks[i];
    chunk.queue = this;
    chunk.reset();
  };
  event = device.newSharedEvent();
  // gw2 (#29): the device's residency set on this queue (DXMT_RESIDENCY_SET); logs once per queue
  gw2::residencyAttachQueue(device, commandQueue);
  // gw2 (#32): DXMT_METAL4 (on by default), decided once per process; with it =0 this does nothing
  gw2::metal4AttachQueue(device);
  if (gw2::metal4Active()) {
    auto mtl4 = std::make_unique<gw2::Mtl4Queue>(device, chunk_count_);
    if (!mtl4->valid())
      gw2::metal4Failed("creating the Metal 4 queue, its event or its allocators");
    else if (!gw2::residencyAttachMtl4Queue(device, mtl4->queue()))
      gw2::metal4Failed("attaching the residency set to the Metal 4 queue");
    else {
      submission_ = std::make_unique<Metal4Submission>(std::move(mtl4), event); // gw2 (#78)
      metal4_ = true;
      Logger::info(str::format(
          "Metal 4: on (DXMT_METAL4=1, the default): this device's chunks go to an MTL4CommandQueue, with ", chunk_count_,
          " command allocators (one per chunk slot) and commit feedback"
      ));
      // gw2 (#184): the queue holds only the device's set; a queue takes at most 32
      Logger::info("Metal 4: drawables stay resident through their layer's residency set, used by each present's "
                   "command buffer; the queue holds no layer's set");
      // gw2 (#154)
      Logger::info(
          gw2::mtl4TimestampReuse()
              ? "Metal 4: timestamp queries read the chunk's last command buffer, the present's after a present "
                "(DXMT_MTL4_TIMESTAMP_REUSE=1)"
              : "Metal 4: a chunk with timestamp queries commits an empty command buffer for them after its last one "
                "(DXMT_MTL4_TIMESTAMP_REUSE=0)"
      );
    }
  }
  // gw2 (#78): Metal 3, unless the queue is on Metal 4
  if (!submission_)
    submission_ = std::make_unique<Metal3Submission>(
        chunk_count_,
        Metal3Submission::Hooks{
            .new_command_buffer = [this] { return CreateCommandBuffer(); },
            .before_commit = gw2::residencyBeforeCommit,
            .split_buffers = [this](uint64_t seq) { return ledger_.splits(seq); }, // gw2 (#66)
            .wait_at_drain = [] { return gw2::residencyActive() || gw2::lifetime::keepUntilRetired(); },
        }
    );
  // gw2 (#69): which holds the chunks need, now that the queue is on Metal 3 or Metal 4 (before its
  // first chunk; the encode thread reads it only after a commit's release)
  ledger_.setPolicy({.metal4 = metal4_, .unretained = unretained_, .lifetime_check = gw2::lifetime::check()});
  // gw2 (#91): only this submission's tokens retire and drain the chunks' holds
  ledger_.acceptTokensFrom(submission_.get());
  // gw2 (#132): the encoder dependency policy (Metal 4 only), before the context's first encoder
  argument_encoding_ctx.setDependencyPolicy(
      gw2::dependencyCompressionAttach(metal4_), gw2::statsEnabled()
  );

  // gw2 (#8): the ring and the frame latency in effect, once per device
  g_ring_chunks.store(chunk_count_, std::memory_order_relaxed);
  Logger::info(str::format(
      "CommandQueue: chunks=", chunk_count_, kChunks.requested().empty() ? " (default)" : " (DXMT_CHUNKS)",
      " metalbuffers=", metal_buffer_count_, " maxlatency=", max_latency_,
      max_latency_override_ ? " (DXMT_MAX_FRAME_LATENCY, replaces the game's)" : " (until the game sets it)"
  ));

  std::string env = env::getEnvVar("DXMT_CAPTURE_FRAME");

  if (!env.empty()) {
    try {
      capture_state.scheduleNextFrameCapture(std::stoull(env));
    } catch (const std::invalid_argument &) {
    }
  }
}

CommandQueue::~CommandQueue() {
  TRACE("Destructing command queue");
  stopped.store(true);
  ready_for_encode++;
  ready_for_encode.notify_one();
  ready_for_commit++;
  ready_for_commit.notify_one();
  SharedEventListener_destroy(shared_event_listener);
  encodeThread.join();
  finishThread.join();
  DrainChunks(); // gw2 (#91)
  event_listener_thread.join();
  TRACE("Destructed command queue");
}

void
CommandQueue::CommitCurrentChunk() {
  emit_window_.reset(); // gw2 (#148): the window's slab is this chunk's; the next chunk takes its own
  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % chunk_count_];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.frame_ = frame_count;
  chunk.resource_initializer_event_id = initializer.flushToWait();
  auto& statistics = CurrentFrameStatistics();
  statistics.command_buffer_count++;
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();

  auto t0 = clock::now();
  // gw2 (#8): at most chunk_count_ - 1 chunks in flight; a full ring blocks here (a ring stall)
  const uint64_t ring_limit = chunk_count_ - 1;
  bool ring_full = chunk_ongoing.load(std::memory_order_acquire) >= ring_limit;
  if (ring_full)
    chunk_ongoing.wait(ring_limit, std::memory_order_acquire);
  uint64_t in_flight = chunk_ongoing.fetch_add(1, std::memory_order_relaxed) + 1;
  auto t1 = clock::now();
  statistics.commit_interval += (t1 - t0);
  if (gw2::statsEnabled())
    gw2::waitAdd(gw2::Wait::Commit, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

  if (gw2::statsEnabled()) {
    // gw2 (#8): the first commit of a new frame publishes the previous frame's totals
    if (ring_frame_.frame != frame_count) {
      if (ring_frame_.commits)
        PublishRingFrame(ring_frame_);
      ring_frame_ = {.frame = frame_count};
    }
    ring_frame_.commits++;
    ring_frame_.occupancy_sum += in_flight;
    ring_frame_.peak = std::max(ring_frame_.peak, in_flight);
    if (ring_full) {
      ring_frame_.stalls++;
      ring_frame_.stall_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
  }

#else
  CommitChunkInternal(chunk, ready_for_encode.fetch_add(1, std::memory_order_relaxed));
#endif

  cpu_command_allocator.free_blocks(cpu_coherent.signaledValue());
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk, uint64_t seq) {

  auto pool = WMT::MakeAutoreleasePool();

  switch (capture_state.getNextAction(chunk.frame_)) {
  case CaptureState::NextAction::StartCapture: {
    WMTCaptureInfo info;
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    info.capture_object = device;
    info.destination = WMTCaptureDestinationGPUTraceDocument;
    char filename[1024];
    std::time_t now;
    std::time(&now);
    std::strftime(filename, 1024, "_%H'%M'%S_%m-%d-%y.gputrace", std::localtime(&now));
    auto fileUrl = env::getUnixPath(env::getExeBaseName() + "_F." + std::to_string(chunk.frame_) + filename);
    WARN("A new capture will be saved to ", fileUrl);
    info.output_url.set(fileUrl.c_str());

    capture_mgr.startCapture(info);
    break;
  }
  case CaptureState::NextAction::StopCapture: {
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    capture_mgr.stopCapture();
    break;
  }
  case CaptureState::NextAction::Nothing: {
    if (capture_state.shouldCaptureNextFrame()) {
      capture_state.scheduleNextFrameCapture(chunk.frame_ + 1);
    }
    break;
  }
  }

  // gw2 (#5): close the previous frame's GetPipeline waits (COMPILE frame= lines)
  gw2::compileEncodeFrame(chunk.frame_);

  // gw2 (#78): the chunk through the queue's submission (Metal 3 or Metal 4): a wait for the
  // initializer's uploads first (Metal 3 in the chunk's command buffer, Metal 4 at the queue), then the
  // chunk's encoders in the backend's encoding, then what is left open commits (after the residency
  // sets' pending changes)
  submission_->begin(seq);
  if (chunk.resource_initializer_event_id) {
    submission_->waitEvent(initializer.event(), chunk.resource_initializer_event_id);
    gw2::lifetime::use(lifetime_id_, seq, initializer.event(), gw2::lifetime::Kind::Event); // gw2 (#31)
  }
  struct Encode final : ChunkEncoder {
    CommandQueue &queue;
    CommandChunk &chunk;
    uint64_t seq;

    Encode(CommandQueue &queue, CommandChunk &chunk, uint64_t seq) : queue(queue), chunk(chunk), seq(seq) {}

    // Metal 3: the chunk keeps its command buffer (the last one, once encoded) until it resets
    void
    encode(WMT::CommandBuffer &cmdbuf) override {
      chunk.attached_cmdbuf = cmdbuf;
      chunk.encode(cmdbuf, queue.argument_encoding_ctx);
      cmdbuf = chunk.attached_cmdbuf;
    }
    // gw2 (#32): Metal 4 (gw2::Mtl4Encoding: command buffers begin as encoders need them; queue events,
    // the present split and the present pass commit; each commit carries the residency sets' pending
    // changes and its feedback)
    void
    encode(gw2::Mtl4Queue &mtl4) override {
      gw2::Mtl4Encoding encoding(mtl4, seq, [this](uint64_t size) {
        bool resident = false;
        auto [mapped, buffer, offset] = queue.AllocateArgumentBuffer(seq, size, &resident);
        return gw2::Mtl4Encoding::Bytes{buffer, offset, resident};
      });
      chunk.encodeMetal4(encoding, queue.argument_encoding_ctx);
    }
  } encode{*this, chunk, seq};
  submission_->encode(seq, encode);
  submission_->commit(seq);

  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_one();
}

uint32_t
CommandQueue::EncodingThread() {
#if ASYNC_ENCODING
  env::setThreadName("dxmt-encode-thread");
  kThreadPriority.apply(thread_priority_, "dxmt-encode-thread"); // gw2 (#151)
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_encode.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    // perform...
    auto &chunk = chunks[internal_seq % chunk_count_];
    CommitChunkInternal(chunk, internal_seq);
    internal_seq++;
  }
  TRACE("encoder thread gracefully terminates");
#endif
  return 0;
}

uint32_t
CommandQueue::WaitForFinishThread() {
  env::setThreadName("dxmt-finish-thread");
  kThreadPriority.apply(thread_priority_, "dxmt-finish-thread"); // gw2 (#151)
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_commit.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    auto &chunk = chunks[internal_seq % chunk_count_];
    // gw2 (#78): every commit of the chunk completed (Metal 4: and its completion event), with their
    // records (#77); then its report
    bool stats = gw2::statsEnabled();
    auto completed = submission_->wait(internal_seq, chunk.chunk_event_id, stats);
    ReportCompletedChunk(chunk, completed, stats);

    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    RetireChunk(chunk, internal_seq, std::move(completed)); // gw2 (#66); gw2 (#113): the loop's sequence

    internal_seq++;
  }
  TRACE("finishing thread gracefully terminates");
  return 0;
}

// gw2 (#66): the one written retirement of a chunk, in the order the finish thread always used
void
CommandQueue::RetireChunk(CommandChunk &chunk, uint64_t seq, CompletedChunk &&completed) {
  // gw2 (#113): `seq` is the finish loop's (it waited for it), the token's on every normal retirement; a
  // refused token may be empty (sequence 0), so the bookkeeping below never takes the token's
  // gw2 (#31): the GPU is done with every buffer of the chunk; from here its references may go
  gw2::lifetime::completed(lifetime_id_, seq);
  chunk.reset();
  // gw2 (#66, #91): its holds (split-off command buffers, objects, presenters, spatial and temporal
  // scalers), with the submission's completion token for it; then (gw2 #32, #78: after reset, Metal 4's
  // readbacks read the commits' feedback) the token goes back to the submission that issued it: Metal 4
  // resets the ring slot's allocator; Metal 3 forgets the command buffer the chunk released.
  // gw2 (#113): gw2::retireStep. With DXMT_RETIRE_RECOVERY, a refused ledger retirement recovers by
  // sequence (one error line), refused holds go once their last chunk completed, and a refused submission
  // retirement retires the chunk the submission last waited for
  auto step = gw2::retireStep(ledger_, *submission_, seq, std::move(completed), retire_recovery_);
  if (!retire_recovery_) {
    if (step.ledger_refused)
      LedgerRefused("retirement", seq);
    if (step.submission_refused)
      RetirementRefused("ChunkSubmission", "retirement", seq);
  } else {
    auto diagnostic = gw2::retireDiagnostic(step);
    if (diagnostic.line != gw2::RetireLine::None)
      RetireRecovered(seq, step, diagnostic);
    if (step.ledger.refused_holds && !step.ledger_refused && !refused_holds_logged_) {
      refused_holds_logged_ = true;
      Logger::info(str::format("RetirementLedger: released ", step.ledger.refused_holds, " refused holds once chunk ",
                               seq, " completed (DXMT_RETIRE_RECOVERY; logged once per queue)"));
    }
  }
  // gw2 (#113): the bookkeeping retires step.seq, the finish loop's sequence (WaitCommittedRetired waits
  // for it), whatever the token said
  cpu_coherent.signal(step.seq);
  chunk_ongoing.fetch_sub(1, std::memory_order_release);
  chunk_ongoing.notify_one();

  staging_allocator.free_blocks(step.seq);
  copy_temp_allocator.free_blocks(step.seq);
  argbuf_allocator.free_blocks(step.seq);
}

// gw2 (#91, L3): teardown's counterpart of RetireChunk, for both backends, in the order teardown always
// used. The finish thread stopped without retiring every committed chunk: the submission drains first
// (Metal 4 waits for every committed command buffer; Metal 3 for its chunks' buffers when the residency
// set (#29), DXMT_UNRETAINED=1 or DXMT_LIFETIME_CHECK=1 (#31) need it), and its token lets the ledger
// release the holds: chunk by chunk as before (its own state, then its ring slot's holds), the chunks
// past the ring (never used) last.
void
CommandQueue::DrainChunks() {
  auto drained = submission_->drain();
  gw2::lifetime::detachQueue(lifetime_id_); // gw2 (#31): nothing of this queue is in use any more
  if (!ledger_.drain(drained, [this](uint32_t slot) { chunks[slot].reset(); })) {
    // never expected: the holds stay until the ledger goes with the queue; the chunks' own state goes now
    RetirementRefused("RetirementLedger", "drain", 0);
    for (unsigned i = 0; i < chunk_count_; i++)
      chunks[i].reset();
  }
  for (unsigned i = chunk_count_; i < kCommandChunkCountMax; i++) {
    auto &chunk = chunks[i];
    chunk.reset();
  };
}

// gw2 (#66): never expected. A refused hold stays held until the queue goes (never released early); a
// refused retirement releases nothing. gw2 (#113): with DXMT_RETIRE_RECOVERY, see RetireChunk.
void
CommandQueue::LedgerRefused(const char *what, uint64_t seq) {
  RetirementRefused("RetirementLedger", what, seq);
}

// gw2 (#91): the ledger's refusals and the submission's (a token it didn't issue or didn't last give),
// the first 8 per queue
void
CommandQueue::RetirementRefused(const char *who, const char *what, uint64_t seq) {
  auto n = ledger_refusals_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 8)
    return;
  ERR(who, ": refused a ", what, " for chunk ", seq, " (ring slot ", seq % chunk_count_, ", retired up to ",
      ledger_.retired(), ", refusal ", n, "; only the first 8 are logged)");
}

// gw2 (#113): a refused retirement and what DXMT_RETIRE_RECOVERY did, as one line (gw2::retireDiagnostic:
// an error for a new failure, info for one that follows it), naming the ledger's and the submission's
// refusal; the first 8 per queue, counted with the hold and drain refusals
void
CommandQueue::RetireRecovered(uint64_t seq, const gw2::RetireStep &step, const gw2::RetireDiagnostic &diagnostic) {
  auto n = ledger_refusals_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 8)
    return;
  const auto &o = step.ledger;
  std::string text;
  if (diagnostic.ledger_part) {
    if (o.follows)
      text = str::format("RetirementLedger: chunk ", seq, " follows the refused retirement before it (retired up to ",
                         o.before, "); ");
    else
      text = str::format("RetirementLedger: refused a retirement for chunk ", seq, " (ring slot ", seq % chunk_count_,
                         ", retired up to ", o.before, ", refusal ", n, "; only the first 8 are logged); ");
    if (o.recovered)
      text += str::format("recovered: retired up to ", o.after, " by sequence (", o.chunks,
                          " chunks with holds released), ", o.refused_holds, " refused holds released, ",
                          ledger_.refusedHolds(), " still refused");
    else
      text += "not recovered: its token proves no completion (empty or another queue's); the next chunk's recovers";
  }
  if (diagnostic.submission_part) {
    if (!text.empty())
      text += "; ChunkSubmission: refused it too, ";
    else
      text = str::format("ChunkSubmission: refused a retirement for chunk ", seq, " (ring slot ", seq % chunk_count_,
                         ", retired up to ", ledger_.retired(), ", refusal ", n, "; only the first 8 are logged); ");
    text += step.submission_recovered ? "recovered: retired the chunk it last waited for"
                                      : "not recovered: it didn't last wait for this chunk, or retired it already";
  }
  text += " (DXMT_RETIRE_RECOVERY)";
  if (diagnostic.line == gw2::RetireLine::Error)
    ERR(text);
  else
    Logger::info(text);
}

// gw2 (#77): what both finish paths report of a completed chunk, from its records: device errors (the
// last buffer's as upstream's line, then its logs; a split buffer's with the chunk's frame and
// sequence id, through the throttle, then its logs), and with DXMT_STATS the GPU spans (gw2 #3: the
// split buffers', then the last one's) and the last one's end for PRESENT's aftergpu (gw2 #7).
void
CommandQueue::ReportCompletedChunk(CommandChunk &chunk, const CompletedChunk &completed, bool stats) {
  struct Sink {
    CommandQueue &queue;
    uint64_t frame;
    uint64_t seq;

    clock::time_point
    now() {
      return clock::now();
    }
    void
    deviceError(const CommitRecord &record) {
      ERR("Device error at frame ", frame, ": ", WMT::Error(record.error).description().getUTF8String());
    }
    void
    lastLog(const CommitRecord &, const std::string &log) {
      ERR("Frame ", frame, ": ", log);
    }
    void
    splitError(const CommitRecord &record, size_t index, size_t count, uint64_t errors, uint64_t not_logged) {
      ERR("Device error at frame ", frame, " (chunk ", seq, ", split-off buffer ", index + 1, " of ", count,
          ", split error ", errors, ", ", not_logged, " not logged since the last line): ",
          WMT::Error(record.error).description().getUTF8String());
      for (auto &log : record.logs)
        ERR("Frame ", frame, " (chunk ", seq, ", split-off buffer ", index + 1, "): ", log);
    }
    void
    gpuSpan(uint64_t start_ns, uint64_t end_ns) {
      queue.gpu_collector.add(frame, start_ns, end_ns);
    }
    void
    presentGpuEnd(uint64_t end_ns) {
      gw2::presentGpuEnd(frame, end_ns);
    }
  } sink{*this, chunk.frame_, completed.seq()};
  gw2::reportCompletedChunk(completed.records(), completed.order(), stats, split_errors_, sink);
}

void CommandQueue::Retain(uint64_t seq, Allocation* allocation) {
  auto &chunk = chunks[seq % chunk_count_];
  auto &tracker = chunk.ref_tracker;
  gw2::lifetime::useAllocation(lifetime_id_, seq, allocation); // gw2 (#31)
  constexpr size_t block_size = decltype(reftracker_storage_allocator)::block_size;
  while (unlikely(!tracker.track(allocation))) {
    auto [temp_buffer, _] = reftracker_storage_allocator.allocate(seq, cpu_coherent.signaledValue(), block_size, 1);
    tracker.addStorage(temp_buffer.ptr, block_size);
  }
};

} // namespace dxmt
