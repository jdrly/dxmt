/*
 * gw2 (#293): my fork's Metal 4 wrappers (#32), out of Metal.hpp because upstream adds to
 * Metal.hpp's CommandQueue and right after it, which is where they were. Metal.hpp never includes
 * this file: a fork header that needs a wrapper includes it (dxmt_gw2_mtl4_queue.hpp,
 * dxmt_gw2_mtl4_encoding.hpp, dxmt_gw2_residency.hpp), and upstream's files get it through those.
 */
#pragma once

#include "./Metal.hpp"

namespace WMT {

/*
 * gw2 (#32): Metal 4 (DXMT_METAL4). The encoders take today's packets and the bridge replays them on
 * Metal 4 encoders, so DXMT's encoding code stays the same; bindings go into the argument tables of
 * a WMTMtl4BindingState the encode thread owns (winemetal.h). Encoders are autoreleased handles like
 * Metal 3's; queues, allocators, command buffers and tables are created +1 (the functions at this file's end).
 */
class MTL4ArgumentTable : public Object {};

class MTL4CommandAllocator : public Object {
public:
  // its command buffers' work must have completed; returns the memory it held
  uint64_t
  reset() {
    return MTL4CommandAllocator_reset(handle);
  }
};

class MTL4CommandEncoder : public CommandEncoder {
public:
  WMTMtl4BindingState *state = nullptr;
  // gw2 (#32 part 3): gives state a new bytes buffer of at least state->bytes_needed when a replay
  // stopped at a Set*Bytes that didn't fit (state->resume); null: such a packet is skipped (counted)
  void (*refill)(void *context, WMTMtl4BindingState *state) = nullptr;
  void *refill_context = nullptr;

protected:
  // a replay call, resumed with a new bytes buffer as often as it stops for one
  template <typename Replay>
  void
  replay(Replay call, const wmtcmd_base *head) {
    for (;;) {
      call(handle, state, head);
      auto resume = state ? (const wmtcmd_base *)state->resume.get_accessible_or_null() : nullptr;
      if (!resume)
        return;
      state->resume.set(nullptr);
      if (refill) {
        refill(refill_context, state);
        head = resume;
      } else {
        state->bytes_overflow++;
        head = (const wmtcmd_base *)const_cast<wmtcmd_base *>(resume)->next.get_accessible_or_null();
        if (!head)
          return;
      }
    }
  }

public:
  void
  barrier(WMTMtl4BarrierScope scope, uint64_t after_stages, uint64_t before_stages,
          uint64_t visibility = WMTMtl4VisibilityDevice) {
    MTL4CommandEncoder_barrier(handle, scope, after_stages, before_stages, visibility);
  }
};

class MTL4RenderCommandEncoder : public MTL4CommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_render_nop *cmd_head) {
    replay(MTL4RenderCommandEncoder_encodeCommands, (const wmtcmd_base *)cmd_head);
  }

  // gw2 (#153): flags: WMT_CMD_RESIDENT when the device's residency set holds the buffer
  void
  setVertexBuffer(Buffer buffer, uint64_t offset, uint8_t index, uint16_t flags = 0) {
    setBuffer(WMTRenderCommandSetVertexBuffer, buffer, offset, index, flags);
  }

  void
  setFragmentBuffer(Buffer buffer, uint64_t offset, uint8_t index, uint16_t flags = 0) {
    setBuffer(WMTRenderCommandSetFragmentBuffer, buffer, offset, index, flags);
  }

  void
  setMeshBuffer(Buffer buffer, uint64_t offset, uint8_t index, uint16_t flags = 0) {
    setBuffer(WMTRenderCommandSetMeshBuffer, buffer, offset, index, flags);
  }

  void
  setObjectBuffer(Buffer buffer, uint64_t offset, uint8_t index, uint16_t flags = 0) {
    setBuffer(WMTRenderCommandSetObjectBuffer, buffer, offset, index, flags);
  }

  void
  setFragmentTexture(Texture texture, uint8_t index) {
    struct wmtcmd_render_settexture cmd;
    cmd.type = WMTRenderCommandSetFragmentTexture;
    cmd.next.set(nullptr);
    cmd.texture = texture;
    cmd.index = index;
    submit(&cmd);
  }

  // copied into state's bytes buffer and bound (Metal 4 has no setBytes)
  void
  setFragmentBytes(const void *buf, uint64_t length, uint8_t index) {
    struct wmtcmd_render_setbytes cmd;
    cmd.type = WMTRenderCommandSetFragmentBytes;
    cmd.next.set(nullptr);
    cmd.bytes.set((void *)buf);
    cmd.length = length;
    cmd.index = index;
    submit(&cmd);
  }

  void
  setRenderPipelineState(RenderPipelineState pso) {
    struct wmtcmd_render_setpso cmd;
    cmd.type = WMTRenderCommandSetPSO;
    setPacketFlags(cmd, 0); // gw2 (#153): not WMT_CMD_RESIDENT
    cmd.next.set(nullptr);
    cmd.pso = pso;
    submit(&cmd);
  }

  void
  setViewport(WMTViewport viewport) {
    struct wmtcmd_render_setviewport cmd;
    cmd.type = WMTRenderCommandSetViewport;
    cmd.next.set(nullptr);
    cmd.viewport = viewport;
    submit(&cmd);
  }

  void
  drawPrimitives(WMTPrimitiveType primitive_type, uint32_t vertex_start, uint32_t vertex_count) {
    struct wmtcmd_render_draw cmd;
    cmd.type = WMTRenderCommandDraw;
    cmd.next.set(nullptr);
    cmd.primitive_type = primitive_type;
    cmd.vertex_start = vertex_start;
    cmd.vertex_count = vertex_count;
    cmd.base_instance = 0;
    cmd.instance_count = 1;
    submit(&cmd);
  }

  void
  memoryBarrier(WMTBarrierScope scope, WMTRenderStages stages_after, WMTRenderStages stages_before) {
    struct wmtcmd_render_memory_barrier cmd;
    cmd.type = WMTRenderCommandMemoryBarrier;
    cmd.next.set(nullptr);
    cmd.scope = scope;
    cmd.stages_before = stages_before;
    cmd.stages_after = stages_after;
    submit(&cmd);
  }

  void
  waitForFence(Fence fence, WMTRenderStages before) {
    fenceOp(WMTRenderCommandWaitForFence, fence, before);
  }

  void
  updateFence(Fence fence, WMTRenderStages after) {
    fenceOp(WMTRenderCommandUpdateFence, fence, after);
  }

  // gw2 (#32 part 3): the resource goes into state's residency set (Metal 4 has no useResource)
  void
  useResource(Resource resource, WMTResourceUsage usage, WMTRenderStages stages) {
    struct wmtcmd_render_useresource cmd;
    cmd.type = WMTRenderCommandUseResource;
    cmd.next.set(nullptr);
    cmd.resource = resource;
    cmd.usage = usage;
    cmd.stages = stages;
    submit(&cmd);
  }

private:
  template <typename T>
  void
  submit(const T *cmd) {
    replay(MTL4RenderCommandEncoder_encodeCommands, (const wmtcmd_base *)cmd);
  }

  void
  setBuffer(WMTRenderCommandType type, Buffer buffer, uint64_t offset, uint8_t index, uint16_t flags) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = type;
    setPacketFlags(cmd, flags); // gw2 (#153)
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    submit(&cmd);
  }

  void
  fenceOp(WMTRenderCommandType type, Fence fence, WMTRenderStages stages) {
    struct wmtcmd_render_fence_op cmd;
    cmd.type = type;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    cmd.stages = stages;
    submit(&cmd);
  }
};

class MTL4ComputeCommandEncoder : public MTL4CommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_compute_nop *cmd_head) {
    replay(MTL4ComputeCommandEncoder_encodeCommands, (const wmtcmd_base *)cmd_head);
  }

  // today's blit packets (Metal 4 has no blit encoder)
  void
  encodeBlitCommands(const wmtcmd_blit_nop *cmd_head) {
    MTL4ComputeCommandEncoder_encodeBlitCommands(handle, state, (const wmtcmd_base *)cmd_head);
  }

  void
  waitForFence(Fence fence, bool blit = false) {
    fenceOp(fence, blit, true);
  }

  void
  updateFence(Fence fence, bool blit = false) {
    fenceOp(fence, blit, false);
  }

private:
  void
  fenceOp(Fence fence, bool blit, bool wait) {
    if (blit) {
      struct wmtcmd_blit_fence_op cmd;
      cmd.type = wait ? WMTBlitCommandWaitForFence : WMTBlitCommandUpdateFence;
      cmd.next.set(nullptr);
      cmd.fence = fence.handle;
      MTL4ComputeCommandEncoder_encodeBlitCommands(handle, state, (const wmtcmd_base *)&cmd);
    } else {
      struct wmtcmd_compute_fence_op cmd;
      cmd.type = wait ? WMTComputeCommandWaitForFence : WMTComputeCommandUpdateFence;
      cmd.next.set(nullptr);
      cmd.fence = fence.handle;
      MTL4ComputeCommandEncoder_encodeCommands(handle, state, (const wmtcmd_base *)&cmd);
    }
  }
};

// gw2 (#32 part 3): a Metal 4 compute encoder that takes today's blit packets, shaped like
// BlitCommandEncoder (Metal 4 has no blit encoder)
class MTL4BlitCommandEncoder : public MTL4ComputeCommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_blit_nop *cmd_head) {
    encodeBlitCommands(cmd_head);
  }

  void
  waitForFence(Fence fence) {
    MTL4ComputeCommandEncoder::waitForFence(fence, true);
  }

  void
  updateFence(Fence fence) {
    MTL4ComputeCommandEncoder::updateFence(fence, true);
  }
};

class MTL4CommandBuffer : public Object {
public:
  void
  begin(MTL4CommandAllocator allocator) {
    MTL4CommandBuffer_beginCommandBuffer(handle, allocator);
  }

  void
  end() {
    MTL4CommandBuffer_endCommandBuffer(handle);
  }

  void
  useResidencySet(ResidencySet residency_set) {
    MTL4CommandBuffer_useResidencySet(handle, residency_set);
  }

  // gw2 (#56): an MTL4FXSpatialScaler's work, outside any encoder; color's and output's memory go
  // into residency_set
  void
  encodeSpatialScale(FXSpatialScaler scaler, Texture color, Texture output, Fence fence, ResidencySet residency_set) {
    MTL4CommandBuffer_encodeSpatialScale(handle, scaler, color, output, fence, residency_set);
  }

  MTL4RenderCommandEncoder
  renderCommandEncoder(const WMTRenderPassInfo &info, WMTMtl4BindingState *state) {
    MTL4RenderCommandEncoder encoder;
    encoder.handle = MTL4CommandBuffer_renderCommandEncoder(handle, &info, state);
    encoder.state = state;
    return encoder;
  }

  MTL4ComputeCommandEncoder
  computeCommandEncoder(WMTMtl4BindingState *state) {
    MTL4ComputeCommandEncoder encoder;
    encoder.handle = MTL4CommandBuffer_computeCommandEncoder(handle, state);
    encoder.state = state;
    return encoder;
  }
};

class MTL4CommandQueue : public Object {
public:
  // feedback: filled by Metal when the buffers complete, then event is set to event_value
  void
  commit(const MTL4CommandBuffer *cmdbufs, uint64_t count, WMTMtl4CommitFeedback *feedback = nullptr,
         Event event = {}, uint64_t event_value = 0) {
    MTL4CommandQueue_commit(
        handle, reinterpret_cast<const obj_handle_t *>(cmdbufs), count, feedback, event.handle, event_value
    );
  }

  void
  waitForDrawable(MetalDrawable drawable) {
    MTL4CommandQueue_waitForDrawable(handle, drawable);
  }

  void
  signalDrawable(MetalDrawable drawable) {
    MTL4CommandQueue_signalDrawable(handle, drawable);
  }

  void
  signalEvent(Event event, uint64_t value) {
    MTL4CommandQueue_signalEvent(handle, event, value);
  }

  void
  waitForEvent(Event event, uint64_t value) {
    MTL4CommandQueue_waitForEvent(handle, event, value);
  }

  void
  addResidencySet(ResidencySet residency_set) {
    MTL4CommandQueue_addResidencySet(handle, residency_set);
  }

  void
  removeResidencySet(ResidencySet residency_set) {
    MTL4CommandQueue_removeResidencySet(handle, residency_set);
  }
};

// gw2 (#32): the +1 creators. They were Device's methods; Device is defined in Metal.hpp, before
// the classes above exist, so they are functions that take the device
inline Reference<MTL4CommandQueue>
newMTL4CommandQueue(Device device) {
  return Reference<MTL4CommandQueue>(MTLDevice_newMTL4CommandQueue(device.handle));
}

inline Reference<MTL4CommandAllocator>
newCommandAllocator(Device device) {
  return Reference<MTL4CommandAllocator>(MTLDevice_newCommandAllocator(device.handle));
}

inline Reference<MTL4CommandBuffer>
newMTL4CommandBuffer(Device device) {
  return Reference<MTL4CommandBuffer>(MTLDevice_newMTL4CommandBuffer(device.handle));
}

inline Reference<MTL4ArgumentTable>
newArgumentTable(Device device, const WMTArgTableInfo &info, Error &error) {
  return Reference<MTL4ArgumentTable>(MTLDevice_newArgumentTable(device.handle, &info, &error.handle));
}

} // namespace WMT
