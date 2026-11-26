// gw2 (#353): a present's fences with DXMT_MTL4_PRESENT=mtl3 (dxmt_gw2_present_fences.hpp, ADR 0018).
// The bodies of the hooks in ArgumentEncodingContext::flushCommandsImpl (encode thread, Metal 4 only)
// and CommandQueue::ReportCompletedChunk (finish thread).

#include "dxmt_command_queue.hpp"
#include "dxmt_gw2_metal4.hpp"
#include "dxmt_gw2_present_fences.hpp"
#include "log/log.hpp"
#include <algorithm>

namespace dxmt {

// Before an encoder is encoded: whether it waits on the fence of a present whose fences never reached
// the queue. With DXMT_STATS, DEPS presentstale counts it.
void
ArgumentEncodingContext::gw2PresentFencesScan(EncoderData *current, bool deps_counting) {
  if (current->type == EncoderType::Null || current->type == EncoderType::Present)
    return;
  bool stale = current->type == EncoderType::Render
                   ? gw2_present_fences_.encoder(
                         current->id, current->fence_wait, &static_cast<RenderEncoderData *>(current)->fence_wait_vertex
                     )
                   : gw2_present_fences_.encoder(current->id, current->fence_wait);
  if (stale && deps_counting)
    dependencyCounters().present_stale.fetch_add(1, std::memory_order_relaxed);
}

// DXMT_MTL4_PRESENT_FENCES=1, at the present, before the split's commit: with mtl3 the pass encodes no
// fences, so an encoder of the Metal 4 queue waits on its waits and updates its fence, in the frame's
// commit. The fence then says that what the present waited for has finished, not that the pass ran:
// the back buffer's writers still wait for `shown`. Should the Metal 3 buffer be missing afterwards,
// the Metal 4 pass encodes them again, which orders nothing differently.
void
ArgumentEncodingContext::gw2PresentFencesRelay(gw2::Mtl4Encoding &cmdbuf, PresentData *data) {
  if (!gw2_present_fences_.emit() || !cmdbuf.queue().metal3Present())
    return;
  auto encoder = cmdbuf.blitCommandEncoder();
  data->fence_wait.forEach([&](auto id) { encoder.waitForFence(fence_pool_[id]); });
  data->fence_update.forEach([&](auto id) { encoder.updateFence(fence_pool_[id]); });
  encoder.endEncoding();
}

// The present in the chunk's record, once its kind is known; with DXMT_STATS, DEPS presentcp counts an
// unreached one on a checkpoint
void
ArgumentEncodingContext::gw2PresentFencesPresented(PresentData *data, bool mtl3, uint64_t value, bool deps_counting) {
  if (gw2_present_fences_.presented(data->id, data->gw2_checkpoint, mtl3, value) && data->gw2_checkpoint &&
      deps_counting)
    dependencyCounters().present_checkpoints.fetch_add(1, std::memory_order_relaxed);
}

// At the chunk's end: its commits, and its record for the finish thread
void
ArgumentEncodingContext::gw2PresentFencesChunk(gw2::Mtl4Encoding &cmdbuf, QueryReadbacks &readbacks) {
  for (auto &segment : cmdbuf.segments())
    gw2_present_fences_.commitBegan(uint32_t(segment.begin / 2));
  readbacks.present_fences = gw2_present_fences_.chunk();
}

// Finish thread, Metal 4: what ordered the chunk, at a device error and in the chunk after it
void
CommandQueue::gw2ReportPresentFences(CommandChunk &chunk, const CompletedChunk &completed) {
  if (!metal4_)
    return;
  auto records = completed.records();
  bool error = std::any_of(records.begin(), records.end(), [](const auto &record) { return bool(record.error); });
  for (auto &line : present_fences_.completed(
           completed.seq(), chunk.frame_, chunk.readback.present_fences, error, gw2::mtl4PresentFencesState()
       ))
    Logger::err(line);
}

} // namespace dxmt
