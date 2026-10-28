#include "config/config.hpp"
#include "dxmt_gw2_stats.hpp"
#include "d3d11_context_state.hpp"
#include "d3d11_fence.hpp"
#include "d3d11_private.h"
#include "d3d11_query.hpp"
#include "dxmt_command_queue.hpp"
#include "d3d11_context_impl.cpp"
#include "dxmt_context.hpp"
#include "dxmt_staging.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>
#include <cstdlib>

namespace dxmt {
struct ContextInternalState {
  using device_mutex_t = d3d11_device_mutex;
  CommandQueue &cmd_queue;
  bool has_dirty_op_since_last_event = false;
};


template<typename Object> Rc<Object> forward_rc(Rc<Object>& obj) {
  return std::move(obj);
}

using ImmediateContextBase = MTLD3D11DeviceContextImplBase<ContextInternalState>;

template <>
template <CommandWithContext<ArgumentEncodingContext> cmd>
void
ImmediateContextBase::EmitST(cmd &&fn) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  chk->emitcc(std::forward<cmd>(fn));
}

template <>
template <CommandWithContext<ArgumentEncodingContext> cmd>
void
ImmediateContextBase::EmitOP(cmd &&fn) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  chk->emitcc(std::forward<cmd>(fn));
  ctx_state.has_dirty_op_since_last_event = true;
}

template <>
template <typename T>
moveonly_list<T>
ImmediateContextBase::AllocateCommandData(size_t n) {
  CommandChunk *chk = ctx_state.cmd_queue.CurrentChunk();
  return moveonly_list<T>((T *)chk->allocate_cpu_heap(sizeof(T) * n, alignof(T)), n);
}

template <>
std::tuple<WMT::Buffer, uint64_t>
ImmediateContextBase::AllocateStagingBuffer(size_t size, size_t alignment) {
  return ctx_state.cmd_queue.AllocateStagingBuffer(size, alignment);
}

template <>
void
ImmediateContextBase::UseCopyDestination(Rc<StagingResource> &staging) {
  staging->useCopyDestination(ctx_state.cmd_queue.CurrentSeqId());
}

template <>
void
ImmediateContextBase::UseCopySource(Rc<StagingResource> &staging) {
  staging->useCopySource(ctx_state.cmd_queue.CurrentSeqId());
}

template <>
std::pair<BufferAllocation *, uint32_t>
ImmediateContextBase::GetDynamicBufferAllocation(Rc<DynamicBuffer> &dynamic) {
  return {dynamic->immediateName().ptr(), dynamic->immediateSuballocation()};
}

class MTLD3D11ImmediateContext : public ImmediateContextBase {
public:
  MTLD3D11ImmediateContext(MTLD3D11Device *pDevice, CommandQueue &cmd_queue) :
      ImmediateContextBase(pDevice, ctx_state, pDevice->mutex),
      cmd_queue(cmd_queue),
      ctx_state({cmd_queue}),
      d3dmt_(this, mutex) {
        ignore_map_flag_no_wait_ = Config::getInstance().getOption<bool>("d3d11.ignoreMapFlagNoWait", false);
        InitPromoteFlush();
      }

  ~MTLD3D11ImmediateContext() {
    if (watchdog_started_) {
      watchdog_stop_.store(true);
      watchdog_armed_.store(~0ull, std::memory_order_release);
      watchdog_armed_.notify_one();
      watchdog_.join();
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(ID3D11Multithread)) {
      *ppvObject = ref(&d3dmt_);
      return S_OK;
    }

    return ImmediateContextBase::QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE
  AddRef() override {
    uint32_t refCount = this->refcount++;
    if (unlikely(!refCount))
      this->m_parent->AddRef();

    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE
  Release() override {
    uint32_t refCount = --this->refcount;
    D3D11_ASSERT(refCount != ~0u && "try to release a 0 reference object");
    if (unlikely(!refCount))
      this->m_parent->Release();

    return refCount;
  }

  void *
  MapDynamicBuffer(Rc<DynamicBuffer> &dynamic, uint64_t current_seq_id, uint64_t coherent_seq_id) {
    if (auto next_sub = dynamic->nextSuballocation()) {
      EmitST([allocation = dynamic->immediateName(), next_sub](ArgumentEncodingContext &enc) mutable {
        allocation->useSuballocation(next_sub);
      });
    } else {
      dynamic->updateImmediateName(current_seq_id, dynamic->allocate(coherent_seq_id), 0, false);
      EmitST([allocation = dynamic->immediateName(),
              buffer = Rc(dynamic->buffer)](ArgumentEncodingContext &enc) mutable {
        allocation->useSuballocation(0);
        auto _ = buffer->rename(forward_rc(allocation));
      });
    }

    return dynamic->immediateMappedMemory();
  }

  HRESULT
  STDMETHODCALLTYPE
  Map(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
      D3D11_MAPPED_SUBRESOURCE *pMappedResource) override {
    GW2_API_SCOPE(Map);
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    if (unlikely(!pResource))
      return E_INVALIDARG;
    UINT buffer_length = 0, &row_pitch = buffer_length;
    UINT bind_flag = 0, &depth_pitch = bind_flag;
    auto current_seq_id = cmd_queue.CurrentSeqId();
    auto coherent_seq_id = cmd_queue.CoherentSeqId();
    if (auto dynamic = GetDynamicBuffer(pResource, &buffer_length, &bind_flag)) {
      if (!pMappedResource)
        return E_INVALIDARG;
      switch (MapType) {
      case D3D11_MAP_READ:
      case D3D11_MAP_WRITE:
      case D3D11_MAP_READ_WRITE:
        return E_INVALIDARG;
      case D3D11_MAP_WRITE_DISCARD: {
        if (bind_flag & D3D11_BIND_VERTEX_BUFFER) {
          state_.InputAssembler.VertexBuffers.set_dirty();
        }
        if (bind_flag & D3D11_BIND_CONSTANT_BUFFER) {
          for (auto &stage : state_.ShaderStages) {
            stage.ConstantBuffers.set_dirty();
          }
        }
        if (bind_flag & D3D11_BIND_SHADER_RESOURCE) {
          for (auto &stage : state_.ShaderStages) {
            stage.SRVs.set_dirty();
          }
        }

        pMappedResource->pData = MapDynamicBuffer(dynamic, current_seq_id, coherent_seq_id);
        pMappedResource->RowPitch = buffer_length;
        pMappedResource->DepthPitch = buffer_length;
        break;
      }
      case D3D11_MAP_WRITE_NO_OVERWRITE: {
        pMappedResource->pData = dynamic->immediateMappedMemory();
        pMappedResource->RowPitch = buffer_length;
        pMappedResource->DepthPitch = buffer_length;
        break;
      }
      }
      return S_OK;
    }
    if (auto dynamic = GetDynamicTexture(pResource, Subresource, &row_pitch, &depth_pitch)) {
      if (!pMappedResource)
        return E_INVALIDARG;
      switch (MapType) {
      case D3D11_MAP_READ:
      case D3D11_MAP_WRITE:
      case D3D11_MAP_READ_WRITE:
      case D3D11_MAP_WRITE_NO_OVERWRITE:
        return E_INVALIDARG;
      case D3D11_MAP_WRITE_DISCARD: {
        for (auto &stage : state_.ShaderStages) {
          stage.SRVs.set_dirty();
        }

        pMappedResource->pData = MapDynamicBuffer(dynamic, current_seq_id, coherent_seq_id);
        pMappedResource->RowPitch = row_pitch;
        pMappedResource->DepthPitch = depth_pitch;
        break;
      }
      }
      return S_OK;
    }
    if (auto dynamic = GetDynamicLinearTexture(pResource, &row_pitch, &depth_pitch)) {
      if (!pMappedResource)
        return E_INVALIDARG;
      switch (MapType) {
      case D3D11_MAP_READ:
      case D3D11_MAP_WRITE:
      case D3D11_MAP_READ_WRITE:
        return E_INVALIDARG;
      case D3D11_MAP_WRITE_DISCARD: {
        for (auto &stage : state_.ShaderStages) {
          stage.SRVs.set_dirty();
        }

        dynamic->updateImmediateName(current_seq_id, dynamic->allocate(coherent_seq_id), false);
        EmitST([allocation = dynamic->immediateName(),
              texture = Rc(dynamic->texture)](ArgumentEncodingContext &enc) mutable {
          auto _ = texture->rename(forward_rc(allocation));
        });

        pMappedResource->pData = dynamic->mappedMemory();
        pMappedResource->RowPitch = row_pitch;
        pMappedResource->DepthPitch = depth_pitch;
        break;
      }
      case D3D11_MAP_WRITE_NO_OVERWRITE: {
        pMappedResource->pData = dynamic->mappedMemory();
        pMappedResource->RowPitch = row_pitch;
        pMappedResource->DepthPitch = depth_pitch;
        break;
      }
      }
      return S_OK;
    }
    if (auto staging = GetStagingResource(pResource, Subresource)) {
      if (!pMappedResource)
        return E_INVALIDARG;

      if (MapType > 3 || MapType == 0)
        return E_INVALIDARG;

      if (ignore_map_flag_no_wait_)
        MapFlags &= ~D3D11_MAP_FLAG_DO_NOT_WAIT;

      while (true) {
        auto result = staging->tryMap(coherent_seq_id, MapType & D3D11_MAP_READ, MapType & D3D11_MAP_WRITE);
        if (result == StagingMapResult::Mapped)
          return E_FAIL;
        if (result == StagingMapResult::Renamable) {
          // when write to a buffer that is gpu-readonly
          auto next_name = staging->allocate(coherent_seq_id);
          // can't guarantee a full overwrite
          std::memcpy(staging->mappedMemory(next_name), staging->mappedImmediateMemory(), staging->length);
          staging->updateImmediateName(current_seq_id, next_name);
          EmitST([staging, next_name](ArgumentEncodingContext &enc) mutable { 
            auto _ = staging->buffer()->rename(staging->allocation(next_name));
          });
          result = StagingMapResult::Mappable;
        }
        if (result == StagingMapResult::Mappable) {
          TRACE("staging map ready");
          pMappedResource->pData = staging->mappedImmediateMemory();
          pMappedResource->RowPitch = staging->bytesPerRow;
          pMappedResource->DepthPitch = staging->bytesPerImage;
          return S_OK;
        }
        if (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT) {
          // gw2 (#4): DXMT_PROMOTE_FLUSH_MIN never leaves a poll waiting on a commit it deferred
          if (deferred_since_ && coherent_seq_id + uint64_t(result) >= cmd_queue.CurrentSeqId())
            CommitDeferred(gw2::CommitWhy::Poll);
          return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        // even it's in a while loop
        // only the first flush will have effect
        // and the following calls are essentially no-op
        gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, gw2::CommitWhy::Map); // gw2 (#4)
        Flush();
        TRACE("staging map block");
        auto& statistics = cmd_queue.CurrentFrameStatistics();
        auto t0 = clock::now();
        cmd_queue.WaitCPUFence(coherent_seq_id + uint64_t(result));
        auto t1 = clock::now();
        statistics.sync_count++;
        statistics.sync_interval += (t1 - t0);
        if (gw2::statsEnabled()) // gw2 (#3): HITCH's sync, with the render thread's interval
          gw2::waitAdd(gw2::Wait::Sync, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        current_seq_id = cmd_queue.CurrentSeqId();
        coherent_seq_id = cmd_queue.CoherentSeqId();
      };
    };
    if (pMappedResource == nullptr) {
      UNIMPLEMENTED("map-on-default: map");
    }
    return E_INVALIDARG;
  }

  void
  STDMETHODCALLTYPE
  Unmap(ID3D11Resource *pResource, UINT Subresource) override {
    GW2_API_SCOPE(Map);
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    if (unlikely(!pResource))
      return;
    UINT row_pitch = 0;
    UINT depth_pitch = 0;
    if (auto staging = GetStagingResource(pResource, Subresource)) {
      staging->unmap();
    };
    if (auto dynamic = GetDynamicTexture(pResource, Subresource, &row_pitch, &depth_pitch)) {
      BlitObject texture(device, pResource);
      UpdateTexture(TextureUpdateCommand(texture, Subresource, nullptr),
                    dynamic->buffer, row_pitch, depth_pitch);
    }
  }

  void
  STDMETHODCALLTYPE
  Begin(ID3D11Asynchronous *pAsync) override {
    if (unlikely(!pAsync))
      return;

    std::lock_guard<d3d11_device_mutex> lock(mutex);

    // in theory pAsync could be any of them: { Query, Predicate, Counter }.
    // However `Predicate` and `Counter` are not supported at all
    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
    case D3D11_QUERY_TIMESTAMP:
    case D3D11_QUERY_EVENT:
      break;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      if (auto query = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->Begin())
        EmitST([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
          enc.beginVisibilityResultQuery(std::move(query));
        });
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      // ignore
      break;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      break;
    }
  }

  // See Begin()
  void
  STDMETHODCALLTYPE
  End(ID3D11Asynchronous *pAsync) override {
    if (unlikely(!pAsync))
      return;

    std::lock_guard<d3d11_device_mutex> lock(mutex);

    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
    case D3D11_QUERY_EVENT: {
      if (ctx_state.has_dirty_op_since_last_event) {
        auto event_id = cmd_queue.GetNextEventSeqId();
        static_cast<MTLD3D11EventQuery *>(pAsync)->Issue(event_id);
        InvalidateCurrentPass(true, gw2::PassClose::Query);
        EmitOP([event_id](ArgumentEncodingContext &enc) mutable {
          enc.signalEvent(event_id);
        });
        // gw2 (#4): a timestamp-disjoint query takes the event path; it counts with the timestamps
        PromoteFlush(desc.Query == D3D11_QUERY_EVENT ? gw2::CommitWhy::Event : gw2::CommitWhy::Timestamp);
        ctx_state.has_dirty_op_since_last_event = false;
      } else {
        static_cast<MTLD3D11EventQuery *>(pAsync)->Issue(cmd_queue.GetCurrentEventSeqId());
      }
      break;
    }
    case D3D11_QUERY_TIMESTAMP: {
      if (auto query = static_cast<MTLD3D11TimestampQuery *>(pAsync)->End()) {
        InvalidateCurrentPass(true, gw2::PassClose::Query);
        EmitOP([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
          enc.sampleTimestamp(std::move(query));
        });
        PromoteFlush(gw2::CommitWhy::Timestamp);
      }
      break;
    }
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      if (auto query = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->End())
        EmitST([query = Rc(query)](ArgumentEncodingContext &enc) mutable {
          enc.endVisibilityResultQuery(std::move(query));
        });
      PromoteFlush(gw2::CommitWhy::Occlusion);
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      // ignore
      break;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      break;
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT GetDataFlags) override {
    if (!pAsync || (DataSize && !pData))
      return E_INVALIDARG;

    // Allow dataSize to be zero
    if (DataSize && DataSize != pAsync->GetDataSize())
      return E_INVALIDARG;

    HRESULT hr = S_FALSE;

    D3D11_QUERY_DESC desc;
    ((ID3D11Query *)pAsync)->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_EVENT:
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      switch (static_cast<MTLD3D11EventQuery *>(pAsync)->CheckEventState(cmd_queue.SignaledEventSeqId())) {
      case EventState::Pending:
        break;
      case EventState::Stall:
        GetDataFlags &= ~D3D11_ASYNC_GETDATA_DONOTFLUSH;
        break;
      case EventState::Signaled:
        hr = S_OK;
        break;
      case EventState::Invalid:
        return DXGI_ERROR_INVALID_CALL;
      }
      break;
    }
    default:
      break;
    }
    switch (desc.Query) {
    case D3D11_QUERY_EVENT: {
      if (pData)
        *static_cast<BOOL *>(pData) = (hr == S_OK);
      break;
    }
    case D3D11_QUERY_OCCLUSION: {
      uint64_t null_data;
      uint64_t *data_ptr = pData ? (uint64_t *)pData : &null_data;
      hr = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_OCCLUSION_PREDICATE: {
      BOOL null_data;
      BOOL *data_ptr = pData ? (BOOL *)pData : &null_data;
      hr = static_cast<MTLD3D11OcclusionQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_TIMESTAMP: {
      uint64_t null_data;
      uint64_t *data_ptr = pData ? (uint64_t *)pData : &null_data;
      hr = static_cast<MTLD3D11TimestampQuery *>(pAsync)->GetData(data_ptr);
      break;
    }
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      if (pData) {
        (*static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT *>(pData)) = {1'000'000'000, FALSE};
      }
      break;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      if (pData) {
        (*static_cast<D3D11_QUERY_DATA_PIPELINE_STATISTICS *>(pData)) = {};
      }
      return S_OK;
    }
    default:
      ERR("Unknown query type ", desc.Query);
      return E_FAIL;
    }
    if (hr == S_FALSE && (GetDataFlags & D3D11_ASYNC_GETDATA_DONOTFLUSH) == 0) {
      cmd_queue.CurrentFrameStatistics().event_stall++;
      // gw2 (#4): the reason is set under the lock Flush takes anyway
      std::lock_guard<d3d11_device_mutex> lock(mutex);
      gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, gw2::CommitWhy::GetData);
      Flush();
    } else if (hr == S_FALSE && promote_min_) {
      // gw2 (#4): a DONOTFLUSH poll that isn't ready commits what DXMT_PROMOTE_FLUSH_MIN deferred,
      // as upstream's speculative commit would already have done (GetData doesn't lock otherwise)
      std::lock_guard<d3d11_device_mutex> lock(mutex);
      CommitDeferred(gw2::CommitWhy::Poll);
    }
    return hr;
  }

  void
  STDMETHODCALLTYPE
  Flush() override {
    Flush1(D3D11_CONTEXT_TYPE_ALL, NULL);
  }

  void
  STDMETHODCALLTYPE
  Flush1(D3D11_CONTEXT_TYPE Type, HANDLE hEvent) override {
    GW2_API_SCOPE(Flush);
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    if (ctx_state.has_dirty_op_since_last_event || promote_flush) {
      gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, gw2::CommitWhy::Flush); // gw2 (#4)
      InvalidateCurrentPass(true, gw2::PassClose::Flush);
      Commit();
    }
    if (hEvent) {
      auto shared_event_listener = cmd_queue.GetSharedEventListener();
      MTLSharedEvent_setWin32EventAtValue(
          cmd_queue.event, shared_event_listener, hEvent, cmd_queue.GetCurrentEventSeqId()
      );
    }
  }

  void PrepareFlush() override {
    InvalidateCurrentPass(true, gw2::PassClose::Present);
  }

  void
  STDMETHODCALLTYPE
  ExecuteCommandList(ID3D11CommandList *pCommandList, BOOL RestoreContextState) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    ResetEncodingContextState();

    Com<MTLD3D11CommandList, false> cmdlist = static_cast<MTLD3D11CommandList *>(pCommandList);
    auto seq_id = ctx_state.cmd_queue.CurrentSeqId();

    // gw2 (#4): upstream replaces a pending promote_flush with the command list's; a commit that
    // DXMT_PROMOTE_FLUSH_MIN deferred stays pending
    if (!deferred_since_) {
      promote_flush = false;
      promote_requests_ = 0;
      promote_nodefer_ = false;
    }
    if (cmdlist->promote_flush)
      PromoteFlush(gw2::CommitWhy::CmdList);

    auto query_list = AllocateCommandData<Rc<VisibilityResultQuery>>(cmdlist->visibility_query_count);
    for (const auto &[query, index] : cmdlist->issued_visibility_query) {
      query_list[index] = new VisibilityResultQuery();
      query->DoDeferredQuery(query_list[index].ptr());
    }

    for (const auto &query : cmdlist->issued_event_query) {
      End(query.ptr());
    }

    for (const auto &query : cmdlist->issued_timestamp_query) {
      End(query.ptr());
    }

    for (const auto &staging : cmdlist->read_staging_resources) {
      staging->useCopySource(seq_id);
    }

    for (const auto &staging : cmdlist->written_staging_resources) {
      staging->useCopyDestination(seq_id);
    }

    for (const auto &used_dynamic : cmdlist->used_dynamic_buffers) {
      if (!used_dynamic.latest)
        continue;
      used_dynamic.buffer->updateImmediateName(seq_id, Rc(used_dynamic.allocation), used_dynamic.suballocation, true);
    }

    for (const auto &used_dynamic : cmdlist->used_dynamic_lineartextures) {
      if (!used_dynamic.latest)
        continue;
      used_dynamic.texture->updateImmediateName(seq_id, Rc(used_dynamic.allocation), true);
    }

    EmitOP([cmdlist = std::move(cmdlist), query_list = std::move(query_list)](ArgumentEncodingContext &enc) {
      // Finished command list should clean up the encoding context
      enc.pushDeferredVisibilityQueries(query_list.data());
      cmdlist->Execute(enc);
      enc.popDeferredVisibilityQueries();
    });

    if (RestoreContextState)
      RestoreEncodingContextState();
    else
      ResetD3D11ContextState();
  }

  HRESULT
  STDMETHODCALLTYPE
  FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList **ppCommandList) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);

    return DXGI_ERROR_INVALID_CALL;
  }

  D3D11_DEVICE_CONTEXT_TYPE
  STDMETHODCALLTYPE
  GetType() override {
    return D3D11_DEVICE_CONTEXT_IMMEDIATE;
  }

  virtual void
  WaitUntilGPUIdle() override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    uint64_t seq = cmd_queue.CurrentSeqId();
    gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, gw2::CommitWhy::Idle); // gw2 (#4)
    if(!InvalidateCurrentPass(false, gw2::PassClose::Flush))
      Commit();
    cmd_queue.WaitCPUFence(seq);
  };

  void
  Commit() override {
    // gw2 (#4): cbwhy: the caller's scope, else what forced a deferred commit (timer), else the
    // request behind promote_flush; "other" should stay 0
    gw2::CommitWhy scope = cmd_queue.gw2_commit_why;
    gw2::CommitWhy why = scope != gw2::CommitWhy::None   ? scope
                         : promote_forced_ != gw2::CommitWhy::None ? promote_forced_
                         : promote_flush                  ? promote_why_
                                                          : gw2::CommitWhy::Other;
    gw2_front_.commit[(unsigned)why]++;
    if (deferred_since_)
      EndDeferral(scope);
    promote_requests_ = 0;
    promote_nodefer_ = false;
    promote_forced_ = gw2::CommitWhy::None;

    promote_flush = false;
    D3D11_ASSERT(cmdbuf_state == CommandBufferState::Idle);
    ctx_state.cmd_queue.CommitCurrentChunk();
    ctx_state.has_dirty_op_since_last_event = false;
    gw2_front_.publish(); // gw2 (#4): once per chunk
  };

  HRESULT STDMETHODCALLTYPE Signal(ID3D11Fence *pFence, UINT64 Value) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto fence = static_cast<MTLD3D11Fence *>(pFence);

    InvalidateCurrentPass(false, gw2::PassClose::Flush);
    EmitOP([event = fence->event, Value](ArgumentEncodingContext &enc) mutable {
      enc.signalEvent(std::move(event), Value);
    });
    gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, gw2::CommitWhy::Fence); // gw2 (#4)
    Flush();

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Wait(ID3D11Fence *pFence, UINT64 Value) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto fence = static_cast<MTLD3D11Fence *>(pFence);

    {
      gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, gw2::CommitWhy::Fence); // gw2 (#4)
      Flush();
    }
    EmitOP([event = fence->event, Value](ArgumentEncodingContext &enc) mutable {
      enc.waitEvent(std::move(event), Value);
    });

    return S_OK;
  }

  UINT
  STDMETHODCALLTYPE
  GetContextFlags() override {
    return 0;
  }

  void
  STDMETHODCALLTYPE
  SwapDeviceContextState(ID3DDeviceContextState *pState, ID3DDeviceContextState **ppPreviousState) override {
    InitReturnPtr(ppPreviousState);

    if (!pState)
      return;

    std::lock_guard<d3d11_device_mutex> lock(mutex);

    ResetEncodingContextState();

    if (!state_object_)
      state_object_ = new MTLD3D11DeviceContextState(device);
    
    state_object_->state = std::move(state_);

    if (ppPreviousState)
      *ppPreviousState = state_object_.ref();

    state_object_ = static_cast<MTLD3D11DeviceContextState *>(pState);
    state_ = std::move(state_object_->state);
    
    RestoreEncodingContextState();
  }

  HRESULT
  AcquireSync(ID3D11Resource *pResource, UINT64 Key, DWORD dwMilliseconds) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto resource = GetResourceCommon(pResource);
    if (!resource->keyedMutex())
      return DXGI_ERROR_INVALID_CALL;
    return resource->keyedMutex()->acquire(
        [&](uint64_t fence) {
          gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, gw2::CommitWhy::KeyedMutex); // gw2 (#4)
          InvalidateCurrentPass(true, gw2::PassClose::Flush);
          EmitOP([event = resource->keyedMutex()->sharedEvent(), fence](ArgumentEncodingContext &enc) mutable {
            enc.waitEvent(std::move(event), fence);
          });
          Flush();
        },
        Key, dwMilliseconds
    );
  }
  HRESULT
  ReleaseSync(ID3D11Resource *pResource, UINT64 Key) override {
    std::lock_guard<d3d11_device_mutex> lock(mutex);
    auto resource = GetResourceCommon(pResource);
    if (!resource->keyedMutex())
      return DXGI_ERROR_INVALID_CALL;
    return resource->keyedMutex()->release(
        [&](uint64_t fence) {
          gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, gw2::CommitWhy::KeyedMutex); // gw2 (#4)
          InvalidateCurrentPass(true, gw2::PassClose::Flush);
          EmitOP([event = resource->keyedMutex()->sharedEvent(), fence](ArgumentEncodingContext &enc) mutable {
            enc.signalEvent(std::move(event), fence);
          });
          Flush();
        },
        Key
    );
  }

  // gw2 (#4): DXMT_PROMOTE_FLUSH_MIN=<n> (0, the default: upstream). A speculative commit (the
  // promote_flush of an occlusion or timestamp query End) is held back at the pass change where
  // upstream commits it, until n requests have accumulated. It commits at once when a request that is
  // never deferred joins (event, staging copy, readback, command list), or when no earlier chunk is
  // still on the GPU (no starvation). The progress guarantee, for a commit it held back:
  // - every way the game can see a result commits it first: Present; explicit Flush, fences, keyed
  //   mutex, Map/GetData waits (untouched, they commit); a GetData(DONOTFLUSH) that isn't ready, and
  //   a Map(DO_NOT_WAIT) that would wait for the held chunk (upstream had already committed);
  // - the timer: the next pass change at least DXMT_PROMOTE_FLUSH_MS (default 2) after the first
  //   deferral commits;
  // - the watchdog: with a multithread-protected device (ID3D11Multithread), whose lock every entry
  //   point takes, a thread commits under that lock DXMT_PROMOTE_FLUSH_MS after the deferral if it
  //   is still pending. Without protection the lock is a no-op and nothing may enter the context
  //   from another thread, so there is no watchdog and the timer acts at the next pass change. A
  //   chunk never spans frames.
  void
  InitPromoteFlush() {
    auto min = env::getEnvVar("DXMT_PROMOTE_FLUSH_MIN");
    promote_min_ = min.empty() ? 0 : (uint32_t)std::strtoul(min.c_str(), nullptr, 10);
    auto ms = env::getEnvVar("DXMT_PROMOTE_FLUSH_MS");
    promote_timer_ms_ = ms.empty() ? 2 : std::max(1ul, std::strtoul(ms.c_str(), nullptr, 10));
    gw2::setPromoteMin(promote_min_);
    if (promote_min_)
      Logger::info(str::format(
          "Promote flush: a speculative commit (occlusion/timestamp query End) waits for ", promote_min_,
          " requests, at most ", promote_timer_ms_,
          " ms or until Present, a poll that isn't ready, a non-deferrable request or an idle GPU "
          "(DXMT_PROMOTE_FLUSH_MIN, DXMT_PROMOTE_FLUSH_MS)"
      ));
  }

  bool
  DeferPromotedCommit() override {
    if (!promote_min_)
      return false;
    auto commit = [&](gw2::Promote what) {
      gw2_front_.promote[(unsigned)what]++;
      return false;
    };
    if (promote_nodefer_)
      return commit(gw2::Promote::NoDefer);
    if (promote_requests_ >= promote_min_)
      return commit(gw2::Promote::Reached);
    // every committed chunk has finished: the GPU would idle while we wait
    if (cmd_queue.CoherentSeqId() + 1 >= cmd_queue.CurrentSeqId())
      return commit(gw2::Promote::GpuIdle);
    int64_t now = gw2::nowNs();
    if (!deferred_since_) {
      deferred_since_ = now;
      ArmWatchdog();
    } else if (now - deferred_since_ >= int64_t(promote_timer_ms_) * 1'000'000) {
      promote_forced_ = gw2::CommitWhy::Timer;
      return commit(gw2::Promote::Timer);
    }
    gw2_front_.promote[(unsigned)gw2::Promote::Skip]++;
    return true;
  }

  // A commit ends a pending deferral; scope is the commit's caller (None: a pass change).
  void
  EndDeferral(gw2::CommitWhy scope) {
    switch (scope) {
    case gw2::CommitWhy::None:
      break; // counted by DeferPromotedCommit
    case gw2::CommitWhy::Present:
      gw2_front_.promote[(unsigned)gw2::Promote::Present]++;
      break;
    case gw2::CommitWhy::Poll:
      gw2_front_.promote[(unsigned)gw2::Promote::Poll]++;
      break;
    case gw2::CommitWhy::Watchdog:
      gw2_front_.promote[(unsigned)gw2::Promote::Watchdog]++;
      break;
    default:
      gw2_front_.promote[(unsigned)gw2::Promote::Explicit]++;
      break;
    }
    deferred_since_ = 0;
    if (watchdog_started_)
      watchdog_armed_.store(0, std::memory_order_release);
  }

  // Commits a deferred speculative commit now (the caller holds the device lock).
  void
  CommitDeferred(gw2::CommitWhy why) {
    if (!deferred_since_)
      return;
    gw2::CommitWhyScope gw2_why(cmd_queue.gw2_commit_why, why);
    InvalidateCurrentPass(true, gw2::PassClose::Flush);
    Commit();
  }

  void
  ArmWatchdog() {
    if (!watchdog_started_) {
      if (!mutex.get_protected()) {
        if (!watchdog_logged_)
          Logger::info("Promote flush: no watchdog, the device isn't multithread-protected; a held commit "
                       "waits for the next pass change, poll or Present");
        watchdog_logged_ = true;
        return;
      }
      Logger::info(str::format(
          "Promote flush: watchdog on (multithread-protected device), commits a held commit after ",
          promote_timer_ms_, " ms"
      ));
      watchdog_started_ = true;
      watchdog_ = dxmt::thread([this] { WatchdogLoop(); });
    }
    watchdog_armed_.store(++watchdog_gen_, std::memory_order_release);
    watchdog_armed_.notify_one();
  }

  void
  WatchdogLoop() {
    env::setThreadName("dxmt-promote-watchdog");
    for (;;) {
      watchdog_armed_.wait(0, std::memory_order_acquire);
      if (watchdog_stop_.load())
        break;
      uint64_t gen = watchdog_armed_.load(std::memory_order_acquire);
      if (!gen)
        continue;
      Sleep(promote_timer_ms_);
      if (watchdog_stop_.load())
        break;
      if (watchdog_armed_.load(std::memory_order_acquire) != gen)
        continue; // that deferral has ended (a newer one gets its own period)
      if (!mutex.get_protected())
        continue; // protection was turned off: the lock no longer excludes the game's threads
      std::lock_guard<d3d11_device_mutex> lock(mutex);
      if (watchdog_armed_.load(std::memory_order_relaxed) != gen)
        continue;
      CommitDeferred(gw2::CommitWhy::Watchdog);
    }
  }

  uint32_t promote_min_ = 0;
  uint32_t promote_timer_ms_ = 2;
  int64_t deferred_since_ = 0; // when the pending deferral began (gw2::nowNs), 0: none
  gw2::CommitWhy promote_forced_ = gw2::CommitWhy::None;
  bool watchdog_started_ = false;
  bool watchdog_logged_ = false;
  uint64_t watchdog_gen_ = 0;
  std::atomic<uint64_t> watchdog_armed_ = 0; // the pending deferral's generation, 0: none
  std::atomic<bool> watchdog_stop_ = false;
  dxmt::thread watchdog_;

private:
  std::vector<Com<MTLD3D11OcclusionQuery>> pending_occlusion_queries;
  CommandQueue &cmd_queue;
  ContextInternalState ctx_state;
  std::atomic<uint32_t> refcount = 0;
  D3D11Multithread d3dmt_;
  bool ignore_map_flag_no_wait_;
  Com<MTLD3D11DeviceContextState, false> state_object_;
};

std::unique_ptr<MTLD3D11DeviceContextBase>
InitializeImmediateContext(MTLD3D11Device *pDevice, CommandQueue &cmd_queue) {
  return std::make_unique<MTLD3D11ImmediateContext>(pDevice, cmd_queue);
}

}; // namespace dxmt