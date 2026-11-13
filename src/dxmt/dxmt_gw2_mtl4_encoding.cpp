#include "dxmt_gw2_mtl4_encoding.hpp"
#include <algorithm>
#include <cstring>

namespace dxmt::gw2 {

namespace {

// before each encoder, at least this much of the bytes arena is free; a new arena is this large
constexpr uint64_t kBytesReserve = 64 * 1024;
constexpr uint64_t kBytesArena = 256 * 1024;

} // namespace

Mtl4Encoding::Mtl4Encoding(Mtl4Queue &queue, uint64_t seq, BytesAllocator bytes) :
    queue_(queue),
    seq_(seq),
    bytes_(std::move(bytes)) {
  state_.residency_set = queue_.residency(seq_).handle;
}

Mtl4Encoding::~Mtl4Encoding() {
  metal4EncodeCounters(state_);
}

Mtl4Commit &
Mtl4Encoding::open() {
  if (auto *commit = queue_.current(seq_))
    return *commit;
  auto &commit = queue_.begin(seq_);
  segments_.push_back({&commit, position_});
  waited_ = false;
  return commit;
}

void
Mtl4Encoding::commitOpen() {
  if (queue_.current(seq_))
    queue_.commit(seq_);
}

void
Mtl4Encoding::openForTimestamps(bool extra) {
  if (extra || segments_.empty() || waited_)
    open();
}

void
Mtl4Encoding::prepare(Kind kind, uint32_t flags) {
  // a new encoder binds nothing yet (as on Metal 3): no buffer for a Set*BufferOffset to move in
  std::memset(state_.buffer_base, 0, sizeof(state_.buffer_base));
  auto &tables = queue_.tables();
  for (auto &table : state_.tables)
    table = NULL_OBJECT_HANDLE;
  if (kind == Kind::Render) {
    state_.tables[WMTMtl4TableVertex] = tables.vertex.handle;
    state_.tables[WMTMtl4TableFragment] = tables.fragment.handle;
    state_.tables[WMTMtl4TableObject] = tables.object.handle;
    state_.tables[WMTMtl4TableMesh] = tables.mesh.handle;
  } else if (kind == Kind::Compute) {
    state_.tables[WMTMtl4TableCompute] = tables.compute.handle;
  }
  state_.flags = flags | bytes_flags_;
  if (state_.bytes_end - state_.bytes_offset < kBytesReserve)
    newArena(kBytesArena);
}

void
Mtl4Encoding::newArena(uint64_t size) {
  size = std::max(size, kBytesArena);
  auto bytes = bytes_(size);
  state_.bytes_buffer = bytes.buffer.handle;
  state_.bytes_offset = bytes.offset;
  state_.bytes_end = bytes.offset + size;
  bytes_flags_ = bytes.resident ? WMT_MTL4_BYTES_RESIDENT : 0; // gw2 (#153)
  state_.flags = (state_.flags & ~WMT_MTL4_BYTES_RESIDENT) | bytes_flags_;
}

void
Mtl4Encoding::refill(void *context, WMTMtl4BindingState *state) {
  auto *self = static_cast<Mtl4Encoding *>(context);
  self->newArena(state->bytes_needed);
}

template <typename Encoder>
Encoder
Mtl4Encoding::withRefill(Encoder encoder) {
  encoder.refill = &Mtl4Encoding::refill;
  encoder.refill_context = this;
  return encoder;
}

WMT::MTL4RenderCommandEncoder
Mtl4Encoding::renderCommandEncoder(WMTRenderPassInfo &info, uint32_t flags) {
  auto &commit = open();
  prepare(Kind::Render, flags);
  return withRefill(WMT::MTL4CommandBuffer{commit.cmdbuf.handle}.renderCommandEncoder(info, &state_));
}

WMT::MTL4ComputeCommandEncoder
Mtl4Encoding::computeCommandEncoder(bool) {
  // a Metal 4 compute encoder's dispatches may run concurrently, as DXMT's concurrent Metal 3 ones
  auto &commit = open();
  prepare(Kind::Compute);
  return withRefill(WMT::MTL4CommandBuffer{commit.cmdbuf.handle}.computeCommandEncoder(&state_));
}

WMT::MTL4BlitCommandEncoder
Mtl4Encoding::blitCommandEncoder() {
  auto &commit = open();
  prepare(Kind::Blit);
  WMT::MTL4BlitCommandEncoder encoder;
  encoder.handle = MTL4CommandBuffer_computeCommandEncoder(commit.cmdbuf.handle, &state_);
  encoder.state = &state_;
  return withRefill(encoder);
}

void
Mtl4Encoding::encodeSignalEvent(WMT::Event event, uint64_t value) {
  commitOpen();
  queue_.signalEvent(event, value);
}

void
Mtl4Encoding::encodeWaitForEvent(WMT::Event event, uint64_t value) {
  commitOpen();
  queue_.waitForEvent(event, value);
  waited_ = true; // gw2 (#154)
}

void
Mtl4Encoding::encodeSpatialScale(WMT::FXSpatialScaler scaler, WMT::Texture color, WMT::Texture output, WMT::Fence fence) {
  auto &commit = open();
  WMT::MTL4CommandBuffer{commit.cmdbuf.handle}.encodeSpatialScale(
      scaler, color, output, fence, WMT::ResidencySet{state_.residency_set}
  );
}

void
Mtl4Encoding::waitForDrawable(WMT::MetalDrawable drawable) {
  queue_.queue().waitForDrawable(drawable);
}

void
Mtl4Encoding::signalDrawable(WMT::MetalDrawable drawable) {
  signalled_ = queue_.current(seq_); // gw2 (#242): its record stays valid until the chunk retires
  commitOpen();
  queue_.queue().signalDrawable(drawable);
}

void
Mtl4Encoding::present(WMT::MetalDrawable drawable, WMTDrawablePresentMode mode, double value) {
  bool before_pass = signalled_ && !signalled_->done(); // gw2 (#242): read before the call
  drawable.present(mode, value);
  metal4PresentCounted(before_pass, 0);
}

void
Mtl4Encoding::presentWhenDone(WMT::MetalDrawable drawable, WMTDrawablePresentMode mode, double value) {
  open(); // the present pass's (encodeCommandsMetal4 began it), so the present always has a commit
  queue_.commitPresent(seq_, drawable, mode, value);
}

uint64_t
Mtl4Encoding::beginMetal3Present(WMT::CommandBuffer present) {
  uint64_t value = queue_.nextMetal3PresentValue();
  encodeSignalEvent(queue_.frameEvent(), value); // after every commit so far: the frame's work
  present.encodeWaitForEvent(queue_.frameEvent(), value);
  return value;
}

void
Mtl4Encoding::commitMetal3Present(WMT::CommandBuffer present, uint64_t value) {
  present.encodeSignalEvent(queue_.shownEvent(), value);
  present.commit();
  queue_.addMetal3Present(seq_, present, value);
  // nothing is open here (the pass went into `present`): this commits nothing
  encodeWaitForEvent(queue_.shownEvent(), value);
}

} // namespace dxmt::gw2
