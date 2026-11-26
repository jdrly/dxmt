#pragma once

// gw2 (#32 part 3): a chunk's Metal 4 encoding, shaped like the WMT::CommandBuffer that
// ArgumentEncodingContext::flushCommands encodes into on Metal 3, so one templated flushCommands
// serves both.
//
// - Command buffers begin when an encoder needs one (Mtl4Queue::begin, on the chunk slot's
//   allocator) and are committed at a queue-level event (a Metal 4 queue signals and waits between
//   commits, not inside a command buffer), at the present split, and at the end of the chunk.
// - Every encoder replays today's packets (src/winemetal/unix/mtl4.c) through one binding state:
//   the queue's argument tables for its kind (render: vertex, fragment, object, mesh; compute: one;
//   blit: none), the chunk slot's residency set, and a bytes arena from the argument-buffer ring
//   for Set*Bytes (at least 64 KB free before each encoder; a replay that fills it stops at the
//   Set*Bytes that didn't fit and resumes with a new arena; the ring frees them after the chunk).
// - The binding state's counters go to the METAL4 section when the chunk is done.

#include "Metal.hpp"
#include "dxmt_gw2_mtl4_queue.hpp"
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace dxmt::gw2 {

class Mtl4Encoding {
public:
  // size bytes in a shared buffer that stays alive until the chunk retired: (buffer, offset)
  using BytesAllocator = std::function<std::pair<WMT::Buffer, uint64_t>(uint64_t size)>;

  Mtl4Encoding(Mtl4Queue &queue, uint64_t seq, BytesAllocator bytes);
  ~Mtl4Encoding();

  Mtl4Encoding(const Mtl4Encoding &) = delete;
  Mtl4Encoding &operator=(const Mtl4Encoding &) = delete;

  // --- as WMT::CommandBuffer's
  // flags: WMTMtl4BindingState::flags for this encoder (WMT_MTL4_NO_ATTACHMENT_RESIDENCY: the present
  // pass's drawable stays out of the chunk's set; the layer's own set covers it)
  WMT::MTL4RenderCommandEncoder renderCommandEncoder(WMTRenderPassInfo &info, uint32_t flags = 0);
  WMT::MTL4ComputeCommandEncoder computeCommandEncoder(bool concurrent);
  WMT::MTL4BlitCommandEncoder blitCommandEncoder();
  // after the work committed so far (commits the open command buffer first)
  void encodeSignalEvent(WMT::Event event, uint64_t value);
  // before the work committed from now on (commits the open command buffer first)
  void encodeWaitForEvent(WMT::Event event, uint64_t value);
  // gw2 (#56): an MTL4FXSpatialScaler's work into the open command buffer, outside any encoder; color's
  // and output's memory go into the chunk's residency set
  void encodeSpatialScale(WMT::FXSpatialScaler scaler, WMT::Texture color, WMT::Texture output, WMT::Fence fence);

  // --- present (part 4)
  // the command buffers committed from now on wait for the drawable (the present pass's, and the
  // open one if there is one: it isn't committed yet)
  void waitForDrawable(WMT::MetalDrawable drawable);
  // commits the open command buffer (the present pass), then signals the drawable at the queue
  void signalDrawable(WMT::MetalDrawable drawable);

  Mtl4Queue &
  queue() {
    return queue_;
  }

  // --- the chunk
  // the chunk's position in flushCommands (2 x the encoders done), for the timestamp segments
  void
  setPosition(uint64_t position) {
    position_ = position;
  }
  // the open command buffer; begins one if none is open
  Mtl4Commit &open();
  // commits the open command buffer, if any
  void commitOpen();

  // each command buffer begun, with the position it began at (timestamp estimates)
  struct Segment {
    const Mtl4Commit *commit;
    uint64_t begin;
  };
  const std::vector<Segment> &
  segments() const {
    return segments_;
  }

  WMTMtl4BindingState &
  state() {
    return state_;
  }

private:
  enum class Kind { Render, Compute, Blit };
  void prepare(Kind kind, uint32_t flags = 0);
  // a new bytes arena of at least `size` (the old one stays with the chunk until it retires)
  void newArena(uint64_t size);
  static void refill(void *context, WMTMtl4BindingState *state);
  template <typename Encoder> Encoder withRefill(Encoder encoder);

  Mtl4Queue &queue_;
  uint64_t seq_;
  BytesAllocator bytes_;
  uint64_t position_ = 0;
  std::vector<Segment> segments_;
  WMTMtl4BindingState state_{};
};

} // namespace dxmt::gw2
