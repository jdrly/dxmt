#include "dxmt_gw2_lifetime.hpp"
#include "Metal.hpp"
#include "dxmt_allocation.hpp"
#include "dxmt_gw2_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>
#include <cxxabi.h>
#include <cstdlib>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace dxmt::gw2::lifetime {

namespace {

bool
readCheck() {
  return env::getEnvVar("DXMT_LIFETIME_CHECK") == "1";
}

bool
readUnretained() {
  return env::getEnvVar("DXMT_UNRETAINED") == "1";
}

const char *
kindName(Kind kind) {
  switch (kind) {
  case Kind::Buffer:
    return "buffer";
  case Kind::Texture:
    return "texture";
  case Kind::Resource:
    return "resource (useResource)";
  case Kind::Pipeline:
    return "pipeline";
  case Kind::DepthStencilState:
    return "depth-stencil state";
  case Kind::SamplerState:
    return "sampler state";
  case Kind::Fence:
    return "fence";
  case Kind::Event:
    return "event";
  case Kind::Drawable:
    return "drawable";
  case Kind::DrawableTexture:
    return "drawable texture";
  case Kind::CounterSampleBuffer:
    return "counter sample buffer";
  case Kind::IndirectCommandBuffer:
    return "indirect command buffer";
  case Kind::Scaler:
    return "MetalFX scaler";
  case Kind::Count:
    break;
  }
  return "?";
}

// One entry per object the check knows: a Metal object this module holds references to (refs), or
// one a chunk used (seq/queue), or a DXMT allocation a chunk held. Keys are addresses in the one
// process address space (Metal handles are unix pointers, allocations PE pointers), so two live
// objects never share one.
struct Entry {
  uint64_t seq = 0;   // the last chunk that used it, 0: none
  uint32_t queue = 0; // that chunk's queue
  uint32_t refs = 0;  // this module's references to a Metal object (lifetime_ref_hook)
  Kind kind = Kind::Buffer;
};

struct Early {
  uint64_t object;
  Entry entry;
  uint64_t completed;
  std::string type; // the allocation's class, or empty for a Metal object
};

// Never destroyed: objects are still released while static objects are destroyed at exit.
struct State {
  std::mutex mutex;
  std::unordered_map<uint64_t, Entry> objects;
  std::unordered_map<uint32_t, uint64_t> completed; // per live queue: the last chunk the GPU finished
  bool reported[size_t(Kind::Count)] = {};
  std::set<std::string> reported_allocations;
  uint64_t stamps = 0;      // window
  uint64_t early = 0;       // window
  uint64_t early_total = 0; // since the start
  bool logged_on = false;
};

State &
state() {
  static State *s = new State();
  return *s;
}

std::atomic<uint32_t> g_next_queue{1};

// Under the mutex: an object stamped by a chunk its queue's GPU hasn't completed yet. A queue that
// is gone (detached) drained first, so nothing of it is early.
bool
isEarly(State &s, const Entry &e, uint64_t &completed) {
  if (!e.seq)
    return false;
  auto it = s.completed.find(e.queue);
  if (it == s.completed.end())
    return false;
  completed = it->second;
  return e.seq > completed;
}

// Under the mutex: counts it, and returns whether this one is logged (the first of its type).
bool
noteEarly(State &s, const Early &early) {
  s.early++;
  s.early_total++;
  if (!early.type.empty())
    return s.reported_allocations.insert(early.type).second;
  return !std::exchange(s.reported[size_t(early.entry.kind)], true);
}

void
logEarly(const Early &early, uint64_t total) {
  std::string what = early.type.empty() ? std::string(kindName(early.entry.kind)) : "allocation " + early.type;
  Logger::err(std::format(
      "LIFETIME {} 0x{:x} released while chunk {} of queue {} may still use it (GPU completed {}): the first "
      "{} released early, {} early releases so far (DXMT_LIFETIME_CHECK; logged once per type)",
      what, early.object, early.entry.seq, early.entry.queue, early.completed, what, total
  ));
}

#ifdef DXMT_GW2_LIFETIME_CHECK
// lifetime_ref_hook (Metal.hpp): +1 a retain, +2 an adopted (+1 returned) handle, -1 a release
void
refHook(obj_handle_t handle, int delta) {
  if (!handle)
    return;
  auto &s = state();
  Early early{};
  uint64_t total = 0;
  {
    std::lock_guard lock(s.mutex);
    if (delta > 0) {
      auto &e = s.objects[handle];
      // A handle adopted while this module holds no reference to it is a new object (or one this
      // module never counted): a stamp left by an object that had the same address before is stale.
      if (delta == 2 && e.refs == 0)
        e = {};
      e.refs++;
      return;
    }
    auto it = s.objects.find(handle);
    if (it == s.objects.end())
      return;
    auto &e = it->second;
    if (e.refs > 1) {
      e.refs--;
      return;
    }
    // refs 0: a reference this module didn't count (taken in another module); it can't tell
    // whether it is the last, so it keeps the stamp and says nothing.
    if (e.refs == 0)
      return;
    // the last reference this module holds
    Entry last = e;
    s.objects.erase(it);
    uint64_t completed = 0;
    if (!isEarly(s, last, completed))
      return;
    early = {handle, last, completed, {}};
    if (!noteEarly(s, early))
      return;
    total = s.early_total;
  }
  logEarly(early, total);
}
#endif

std::string
lifecheckSection(double, uint32_t) {
  if (!check())
    return {};
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto line = std::format(
      "stamps={} objects={} queues={} early={} total={}", s.stamps, s.objects.size(), s.completed.size(), s.early,
      s.early_total
  );
  s.stamps = 0;
  s.early = 0;
  return line;
}

gw2::SectionRegistrar g_lifecheck_section("LIFECHECK", lifecheckSection);

} // namespace

// only in the diagnostic build: DXMT_LIFETIME_CHECK=1 is ignored otherwise (attachQueue warns)
extern const bool g_check = kBuilt && readCheck();

#ifdef DXMT_GW2_LIFETIME_CHECK
namespace {
struct HookInstaller {
  HookInstaller() {
    if (g_check)
      WMT::lifetime_ref_hook = &refHook;
  }
};
} // namespace

// after g_check: same translation unit, initialised in order
HookInstaller g_hook_installer;
#endif

bool
unretained() {
  static const bool on = readUnretained();
  return on;
}

const char *
unretainedState() {
  return unretained() ? "1" : "0";
}

bool
keepUntilRetired() {
  static const bool on = unretained() || check();
  return on;
}

const char *
checkState() {
  if (g_check)
    return "1";
  return !kBuilt && readCheck() ? "unbuilt" : "0";
}

uint32_t
attachQueue() {
  uint32_t id = g_next_queue.fetch_add(1, std::memory_order_relaxed);
  if (unretained())
    Logger::info(
        "Command buffers: unretained references (DXMT_UNRETAINED=1): a chunk's command buffers don't retain what "
        "they use; DXMT holds it until the finish thread retires the chunk"
    );
  else
    Logger::info("Command buffers: retained references (DXMT_UNRETAINED=0), as upstream");
  if (!kBuilt && readCheck()) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      Logger::warn(
          "Lifetime check: DXMT_LIFETIME_CHECK=1 ignored: this build doesn't have it (a diagnostic build: meson "
          "-Dgw2_lifetime_check=true)"
      );
  }
  if (!check())
    return id;
  auto &s = state();
  bool first;
  {
    std::lock_guard lock(s.mutex);
    s.completed[id] = 0;
    first = !std::exchange(s.logged_on, true);
  }
  if (first)
    Logger::info(
        "Lifetime check: on (DXMT_LIFETIME_CHECK=1): every object a chunk uses is stamped with its sequence; "
        "releasing DXMT's last reference to one before the GPU completed that chunk logs a LIFETIME line (once per "
        "type) and counts in LIFECHECK"
    );
  return id;
}

void
detachQueue(uint32_t queue) {
  if (!g_check)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.completed.erase(queue);
}

void
completedSlow(uint32_t queue, uint64_t seq) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.completed.find(queue);
  if (it != s.completed.end() && seq > it->second)
    it->second = seq;
}

void
useSlow(uint32_t queue, uint64_t seq, obj_handle_t object, Kind kind) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto &e = s.objects[object];
  if (seq >= e.seq || e.queue != queue) {
    e.seq = seq;
    e.queue = queue;
    e.kind = kind;
  }
  s.stamps++;
}

void
useAllocationSlow(uint32_t queue, uint64_t seq, Allocation *allocation) {
  useSlow(queue, seq, (uint64_t)(uintptr_t)allocation, Kind::Buffer);
}

void
allocationFreedSlow(Allocation *allocation) {
  auto &s = state();
  Early early{};
  uint64_t total = 0;
  {
    std::lock_guard lock(s.mutex);
    auto it = s.objects.find((uint64_t)(uintptr_t)allocation);
    if (it == s.objects.end())
      return;
    Entry last = it->second;
    s.objects.erase(it);
    uint64_t completed = 0;
    if (!isEarly(s, last, completed))
      return;
    // the allocation is still whole here (free() comes after): its class names the type
    const char *mangled = typeid(*allocation).name();
    int status = 0;
    char *demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    early = {(uint64_t)(uintptr_t)allocation, last, completed, status == 0 && demangled ? demangled : mangled};
    std::free(demangled);
    if (!noteEarly(s, early))
      return;
    total = s.early_total;
  }
  logEarly(early, total);
}

namespace {

template <typename T>
const T *
as(const wmtcmd_base *cmd) {
  return reinterpret_cast<const T *>(cmd);
}

} // namespace

// The handles each command type carries, as _MTL*CommandEncoder_encodeCommands (winemetal_unix.c)
// reads them.
void
useCommands(uint32_t queue, uint64_t seq, const wmtcmd_base *head, Encoder kind) {
  auto u = [&](obj_handle_t object, Kind k) { use(queue, seq, object, k); };
  for (auto cmd = head; cmd; cmd = (const wmtcmd_base *)cmd->next.ptr) {
    switch (kind) {
    case Encoder::Render:
      switch ((WMTRenderCommandType)cmd->type) {
      case WMTRenderCommandUseResource:
        u(as<wmtcmd_render_useresource>(cmd)->resource, Kind::Resource);
        break;
      case WMTRenderCommandSetVertexBuffer:
      case WMTRenderCommandSetFragmentBuffer:
      case WMTRenderCommandSetMeshBuffer:
      case WMTRenderCommandSetObjectBuffer:
        u(as<wmtcmd_render_setbuffer>(cmd)->buffer, Kind::Buffer);
        break;
      case WMTRenderCommandSetFragmentTexture:
        u(as<wmtcmd_render_settexture>(cmd)->texture, Kind::Texture);
        break;
      case WMTRenderCommandSetPSO:
        u(as<wmtcmd_render_setpso>(cmd)->pso, Kind::Pipeline);
        break;
      case WMTRenderCommandSetDSSO:
        u(as<wmtcmd_render_setdsso>(cmd)->dsso, Kind::DepthStencilState);
        break;
      case WMTRenderCommandSetDepthStencilState:
        u(as<wmtcmd_render_setdepthstencilstate>(cmd)->depth_stencil_state, Kind::DepthStencilState);
        break;
      case WMTRenderCommandDrawIndexed:
        u(as<wmtcmd_render_draw_indexed>(cmd)->index_buffer, Kind::Buffer);
        break;
      case WMTRenderCommandDrawIndirect:
        u(as<wmtcmd_render_draw_indirect>(cmd)->indirect_args_buffer, Kind::Buffer);
        break;
      case WMTRenderCommandDrawIndexedIndirect: {
        auto c = as<wmtcmd_render_draw_indexed_indirect>(cmd);
        u(c->index_buffer, Kind::Buffer);
        u(c->indirect_args_buffer, Kind::Buffer);
        break;
      }
      case WMTRenderCommandDrawMeshThreadgroupsIndirect:
        u(as<wmtcmd_render_draw_meshthreadgroups_indirect>(cmd)->indirect_args_buffer, Kind::Buffer);
        break;
      case WMTRenderCommandDXMTGeometryDrawIndexed:
        u(as<wmtcmd_render_dxmt_geometry_draw_indexed>(cmd)->index_buffer, Kind::Buffer);
        break;
      case WMTRenderCommandDXMTGeometryDrawIndirect: {
        auto c = as<wmtcmd_render_dxmt_geometry_draw_indirect>(cmd);
        u(c->imm_draw_arguments, Kind::Buffer);
        u(c->indirect_args_buffer, Kind::Buffer);
        u(c->dispatch_args_buffer, Kind::Buffer);
        break;
      }
      case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
        auto c = as<wmtcmd_render_dxmt_geometry_draw_indexed_indirect>(cmd);
        u(c->index_buffer, Kind::Buffer);
        u(c->imm_draw_arguments, Kind::Buffer);
        u(c->indirect_args_buffer, Kind::Buffer);
        u(c->dispatch_args_buffer, Kind::Buffer);
        break;
      }
      case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
        u(as<wmtcmd_render_dxmt_tessellation_mesh_draw_indexed>(cmd)->index_buffer, Kind::Buffer);
        break;
      case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
        auto c = as<wmtcmd_render_dxmt_tessellation_mesh_draw_indirect>(cmd);
        u(c->imm_draw_arguments, Kind::Buffer);
        u(c->indirect_args_buffer, Kind::Buffer);
        u(c->dispatch_args_buffer, Kind::Buffer);
        break;
      }
      case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
        auto c = as<wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect>(cmd);
        u(c->imm_draw_arguments, Kind::Buffer);
        u(c->indirect_args_buffer, Kind::Buffer);
        u(c->dispatch_args_buffer, Kind::Buffer);
        u(c->index_buffer, Kind::Buffer);
        break;
      }
      case WMTRenderCommandWaitForFence:
      case WMTRenderCommandUpdateFence:
        u(as<wmtcmd_render_fence_op>(cmd)->fence, Kind::Fence);
        break;
      case WMTRenderCommandExecuteCommandsInBuffer:
        u(as<wmtcmd_render_executecommands>(cmd)->indirect_command_buffer, Kind::IndirectCommandBuffer);
        break;
      default: // no object (offsets, bytes, viewports, rasterizer and blend state, plain draws)
        break;
      }
      break;
    case Encoder::Compute:
      switch ((WMTComputeCommandType)cmd->type) {
      case WMTComputeCommandDispatchIndirect:
        u(as<wmtcmd_compute_dispatch_indirect>(cmd)->indirect_args_buffer, Kind::Buffer);
        break;
      case WMTComputeCommandSetPSO:
        u(as<wmtcmd_compute_setpso>(cmd)->pso, Kind::Pipeline);
        break;
      case WMTComputeCommandSetBuffer:
        u(as<wmtcmd_compute_setbuffer>(cmd)->buffer, Kind::Buffer);
        break;
      case WMTComputeCommandUseResource:
        u(as<wmtcmd_compute_useresource>(cmd)->resource, Kind::Resource);
        break;
      case WMTComputeCommandSetTexture:
        u(as<wmtcmd_compute_settexture>(cmd)->texture, Kind::Texture);
        break;
      case WMTComputeCommandWaitForFence:
      case WMTComputeCommandUpdateFence:
        u(as<wmtcmd_compute_fence_op>(cmd)->fence, Kind::Fence);
        break;
      case WMTComputeCommandExecuteCommandsInBuffer:
        u(as<wmtcmd_compute_executecommands>(cmd)->indirect_command_buffer, Kind::IndirectCommandBuffer);
        break;
      default:
        break;
      }
      break;
    case Encoder::Blit:
      switch ((WMTBlitCommandType)cmd->type) {
      case WMTBlitCommandCopyFromBufferToBuffer: {
        auto c = as<wmtcmd_blit_copy_from_buffer_to_buffer>(cmd);
        u(c->src, Kind::Buffer);
        u(c->dst, Kind::Buffer);
        break;
      }
      case WMTBlitCommandCopyFromBufferToTexture: {
        auto c = as<wmtcmd_blit_copy_from_buffer_to_texture>(cmd);
        u(c->src, Kind::Buffer);
        u(c->dst, Kind::Texture);
        break;
      }
      case WMTBlitCommandCopyFromBufferToTextureWithBlitOption: {
        auto c = as<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>(cmd);
        u(c->src, Kind::Buffer);
        u(c->dst, Kind::Texture);
        break;
      }
      case WMTBlitCommandCopyFromTextureToBuffer: {
        auto c = as<wmtcmd_blit_copy_from_texture_to_buffer>(cmd);
        u(c->src, Kind::Texture);
        u(c->dst, Kind::Buffer);
        break;
      }
      case WMTBlitCommandCopyFromTextureToBufferWithBlitOption: {
        auto c = as<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>(cmd);
        u(c->src, Kind::Texture);
        u(c->dst, Kind::Buffer);
        break;
      }
      case WMTBlitCommandCopyFromTextureToTexture: {
        auto c = as<wmtcmd_blit_copy_from_texture_to_texture>(cmd);
        u(c->src, Kind::Texture);
        u(c->dst, Kind::Texture);
        break;
      }
      case WMTBlitCommandCopyTexture: {
        auto c = as<wmtcmd_blit_copy_texture>(cmd);
        u(c->src, Kind::Texture);
        u(c->dst, Kind::Texture);
        break;
      }
      case WMTBlitCommandGenerateMipmaps:
        u(as<wmtcmd_blit_generate_mipmaps>(cmd)->texture, Kind::Texture);
        break;
      case WMTBlitCommandWaitForFence:
      case WMTBlitCommandUpdateFence:
        u(as<wmtcmd_blit_fence_op>(cmd)->fence, Kind::Fence);
        break;
      case WMTBlitCommandFillBuffer:
        u(as<wmtcmd_blit_fillbuffer>(cmd)->buffer, Kind::Buffer);
        break;
      case WMTBlitCommandResolveCounters: {
        auto c = as<wmtcmd_blit_resolvecounters>(cmd);
        u(c->sample_buffer, Kind::CounterSampleBuffer);
        u(c->dst_buffer, Kind::Buffer);
        break;
      }
      case WMTBlitCommandResetCommandsInBuffer:
        u(as<wmtcmd_blit_resetcommands>(cmd)->indirect_command_buffer, Kind::IndirectCommandBuffer);
        break;
      default:
        break;
      }
      break;
    }
  }
}

} // namespace dxmt::gw2::lifetime
