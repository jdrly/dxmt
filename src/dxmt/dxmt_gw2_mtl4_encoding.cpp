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
  return commit;
}

void
Mtl4Encoding::commitOpen() {
  if (queue_.current(seq_))
    queue_.commit(seq_);
}

void
Mtl4Encoding::prepare(Kind kind) {
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
  state_.flags = 0;
  if (state_.bytes_end - state_.bytes_offset < kBytesReserve)
    newArena(kBytesArena);
}

void
Mtl4Encoding::newArena(uint64_t size) {
  size = std::max(size, kBytesArena);
  auto [buffer, offset] = bytes_(size);
  state_.bytes_buffer = buffer.handle;
  state_.bytes_offset = offset;
  state_.bytes_end = offset + size;
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
Mtl4Encoding::renderCommandEncoder(WMTRenderPassInfo &info) {
  auto &commit = open();
  prepare(Kind::Render);
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
}

} // namespace dxmt::gw2
