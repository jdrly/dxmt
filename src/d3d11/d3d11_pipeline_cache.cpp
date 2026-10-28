#include "d3d11_pipeline_cache.hpp"
#include "airconv_public.h"
#include "d3d11_device.hpp"
#include "d3d11_shader.hpp"
#include "d3d11_pipeline.hpp"
#include "dxmt_gw2_compile.hpp"
#include "dxmt_shader_cache.hpp"
#include "dxmt_tasks.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "util_env.hpp"
#include "../d3d10/d3d10_shader.hpp"
#include "../d3d10/d3d10_input_layout.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <shared_mutex>

namespace dxmt {

// gw2 (#6): the pipeline lookup's switches and its PSO section.
namespace gw2 {

const PsoSettings &
psoSettings() {
  static const PsoSettings settings = {
      .blend_hash = env::getEnvVar("DXMT_PSO_BLEND_HASH") != "0",
      .mru = env::getEnvVar("DXMT_PSO_MRU") != "0",
      .topology_keep = env::getEnvVar("DXMT_PSO_TOPOLOGY_KEEP") != "0",
  };
  return settings;
}

std::atomic<uint64_t> g_pso_counters[(unsigned)PsoCounter::Count];

namespace {

// Sampled lookups (one in 16 per thread, like Draw in PACE): bucket depth and lock time.
constexpr uint32_t kPsoSampleEvery = 16;
thread_local uint32_t t_pso_sample_counter = 0;
std::atomic<uint64_t> g_pso_samples;
std::atomic<uint64_t> g_pso_depth_sum;
std::atomic<uint64_t> g_pso_depth_max;
std::atomic<uint64_t> g_pso_wait_ns;
std::atomic<uint64_t> g_pso_held_ns;

uint64_t
drain(std::atomic<uint64_t> &v) {
  return v.exchange(0, std::memory_order_relaxed);
}

// Constructed before the cache's lock is taken, told when it is held; the destructor runs after
// the lock_guard's (declared later), so `held` ends after the unlock.
class PsoLookupSample {
public:
  PsoLookupSample() {
    if (!statsEnabled())
      return;
    stats_ = true;
    if ((t_pso_sample_counter++ % kPsoSampleEvery) == 0)
      t0_ = nowNs();
  }

  template <typename Map, typename Key>
  void
  locked(const Map &map, const Key &key) {
    if (!stats_)
      return;
    g_pso_counters[(unsigned)PsoCounter::Lookup].fetch_add(1, std::memory_order_relaxed);
    if (!t0_)
      return;
    int64_t acquired = nowNs();
    g_pso_wait_ns.fetch_add(uint64_t(acquired - t0_), std::memory_order_relaxed);
    uint64_t depth = map.bucket_count() ? map.bucket_size(map.bucket(key)) : 0;
    g_pso_samples.fetch_add(1, std::memory_order_relaxed);
    g_pso_depth_sum.fetch_add(depth, std::memory_order_relaxed);
    uint64_t prev = g_pso_depth_max.load(std::memory_order_relaxed);
    while (depth > prev && !g_pso_depth_max.compare_exchange_weak(prev, depth, std::memory_order_relaxed)) {
    }
    t1_ = nowNs(); // the depth probe (one more hash) stays out of `held`
  }

  ~PsoLookupSample() {
    if (!t1_)
      return;
    g_pso_held_ns.fetch_add(uint64_t(nowNs() - t1_), std::memory_order_relaxed);
  }

  PsoLookupSample(const PsoLookupSample &) = delete;
  PsoLookupSample &operator=(const PsoLookupSample &) = delete;

private:
  bool stats_ = false;
  int64_t t0_ = 0;
  int64_t t1_ = 0;
};

// The caches' key equality: equal_to<MTL_GRAPHICS_PIPELINE_DESC>, counted. The context MRU's own
// compares use equal_to directly and are not counted.
struct PsoKeyEqual {
  bool
  operator()(const MTL_GRAPHICS_PIPELINE_DESC &x, const MTL_GRAPHICS_PIPELINE_DESC &y) const {
    psoCount(PsoCounter::Equal);
    return std::equal_to<MTL_GRAPHICS_PIPELINE_DESC>{}(x, y);
  }
};

std::string
psoSection(double window_ms, uint32_t frames) {
  (void)window_ms;
  uint64_t c[(unsigned)PsoCounter::Count];
  for (unsigned i = 0; i < (unsigned)PsoCounter::Count; i++)
    c[i] = drain(g_pso_counters[i]);
  uint64_t samples = drain(g_pso_samples), depth_sum = drain(g_pso_depth_sum), depth_max = drain(g_pso_depth_max);
  double wait = drain(g_pso_wait_ns) * double(kPsoSampleEvery) / 1e6;
  double held = drain(g_pso_held_ns) * double(kPsoSampleEvery) / 1e6;
  auto at = [&](PsoCounter k) { return double(c[(unsigned)k]); };
  if (!at(PsoCounter::Build) && !at(PsoCounter::Lookup) && !at(PsoCounter::TopologyKept))
    return {};
  double n = std::max(frames, 1u);
  return std::format(
      "builds={:.1f} mru={:.1f} lookups={:.1f} equal={:.1f} depth={:.2f}/{} new={} topokept={:.1f} "
      "lock={:.3f}/{:.3f}",
      at(PsoCounter::Build) / n, at(PsoCounter::MruHit) / n, at(PsoCounter::Lookup) / n, at(PsoCounter::Equal) / n,
      samples ? double(depth_sum) / samples : 0.0, depth_max, c[(unsigned)PsoCounter::New],
      at(PsoCounter::TopologyKept) / n, wait / n, held / n
  );
}

SectionRegistrar g_pso_section("PSO", psoSection);

} // namespace
} // namespace gw2

class MTLD3D11InputLayout final
    : public MTLD3D11DeviceChild<IMTLD3D11InputLayout> {
public:
  MTLD3D11InputLayout(MTLD3D11Device *device, ManagedInputLayout input_layout)
      : MTLD3D11DeviceChild<IMTLD3D11InputLayout>(device),
        input_layout(input_layout), d3d10(this) {}

  ~MTLD3D11InputLayout() {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11InputLayout) ||
        riid == __uuidof(IMTLD3D11InputLayout)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10DeviceChild) ||
        riid == __uuidof(ID3D10InputLayout)) {
      *ppvObject = ref(&d3d10);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11InputLayout), riid)) {
      WARN("D3D311InputLayout: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  ManagedInputLayout GetManagedInputLayout() override { return input_layout; }

private:
  ManagedInputLayout input_layout;
  MTLD3D10InputLayout d3d10;
};

template <> struct task_trait<ThreadpoolWork *> {
  ThreadpoolWork *run_task(ThreadpoolWork *task) {
    return task->RunThreadpoolWork();
  }
  bool get_done(ThreadpoolWork *task) { return task->GetIsDone(); }
  void set_done(ThreadpoolWork *task) { task->SetIsDone(true); }
};

class PipelineCache : public MTLD3D11PipelineCacheBase {

  class CachedSM50Shader final : public Shader {
    PipelineCache *cache;
    sm50_shader_t shader = nullptr;
    Sha1Digest sha1_;
    MTL_SHADER_REFLECTION reflection_;
    MTL_SM50_SHADER_ARGUMENT *arguments_info_buffer;
    std::unordered_map<ShaderVariant, std::unique_ptr<CompiledShader>> variants;

  public:
    CachedSM50Shader(PipelineCache *cache, sm50_shader_t shader_transferred,
                     const Sha1Digest &hash, MTL_SHADER_REFLECTION &reflection)
        : cache(cache), shader(shader_transferred), sha1_(hash),
          reflection_(reflection) {
      if (reflection_.NumConstantBuffers + reflection_.NumArguments) {
        arguments_info_buffer = (MTL_SM50_SHADER_ARGUMENT *)malloc(
            sizeof(MTL_SM50_SHADER_ARGUMENT) *
            (reflection_.NumConstantBuffers + reflection_.NumArguments));
        SM50GetArgumentsInfo(shader, arguments_info_buffer,
                             arguments_info_buffer +
                                 reflection_.NumConstantBuffers);
      } else {
        arguments_info_buffer = nullptr;
      }
    }

    ~CachedSM50Shader() {
      if (shader) {
        SM50Destroy(shader);
        if (arguments_info_buffer)
          free(arguments_info_buffer);
        shader = nullptr;
      }
    };

    CachedSM50Shader(CachedSM50Shader &&moved) = delete;
    CachedSM50Shader(const CachedSM50Shader &copy) = delete;

    virtual sm50_shader_t handle() { return shader; };
    virtual MTL_SHADER_REFLECTION &reflection() { return reflection_; }
    virtual MTL_SM50_SHADER_ARGUMENT *constant_buffers_info() {
      return arguments_info_buffer;
    };
    virtual MTL_SM50_SHADER_ARGUMENT *arguments_info() {
      return arguments_info_buffer + reflection_.NumConstantBuffers;
    };
    virtual CompiledShader *get_shader(ShaderVariant variant) {
      auto c = variants.insert({variant, nullptr});
      if (c.second) {
        c.first->second = std::visit(
            [=, this](auto var) {
              return CreateVariantShader(cache->device, this, var);
            },
            variant);
        gw2::compileVariantNew(); // gw2 (#5)
        cache->scheduler_.submit(c.first->second.get());
      }
      return c.first->second.get();
    }
    virtual const Sha1Digest &sha1() { return sha1_; };

#ifdef DXMT_DEBUG
    void *bytecode;
    size_t bytecode_length;

    virtual void dump() {
      std::fstream dump_out;
      dump_out.open("shader_dump_" + sha1_.string() + ".cso",
                    std::ios::out | std::ios::binary);
      if (dump_out) {
        dump_out.write((char *)bytecode, bytecode_length);
      }
      dump_out.close();
      WARN("shader dumped to ./shader_dump_" + sha1_.string() + ".cso");
    }
#else
    virtual void dump() {}
#endif

    virtual WMT::Reference<WMT::DispatchData> find_cached_variant(Sha1Digest &variant_digest) final {
      auto reader = cache->scache_.getReader();
      if (reader)
        return reader->get(std::make_pair(sha1_, variant_digest));
      return {};
    };
    virtual void update_cached_variant(Sha1Digest &variant_digest, WMT::DispatchData data) final {
      auto writer = cache->scache_.getWriter();
      if (writer)
        writer->set(std::make_pair(sha1_, variant_digest), data);
    }
  };

  class CachedInputLayout final : public InputLayout {
  private:
  public:
    CachedInputLayout(
        std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> &&attributes,
        uint32_t input_slot_mask)
        : attributes_(attributes), input_slot_mask_(input_slot_mask) {
      Sha1HashState h;
      h.update(input_slot_mask);
      h.update(attributes_.size());
      for (auto &el : attributes_) {
        h.update(el);
      }
      sha1_ = h.final();
    }

    virtual uint32_t input_slot_mask() final { return input_slot_mask_; }

    virtual uint32_t input_layout_element(
        MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC **ppElements) final {
      *ppElements = attributes_.data();
      return attributes_.size();
    }

    virtual Sha1Digest &sha1() final { return sha1_; }

    std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> attributes_;
    Sha1Digest sha1_;
    uint32_t input_slot_mask_;
  };

  ShaderCache& scache_;

  task_scheduler<ThreadpoolWork *> scheduler_;

  MTLD3D11Device *device;
  StateObjectCache<D3D11_BLEND_DESC1, IMTLD3D11BlendState> blend_states;

  std::unordered_map<MTL_INPUT_LAYOUT_DESC, std::unique_ptr<CachedInputLayout>> input_layouts;
  dxmt::mutex mutex_ia_;

  StateObjectCache<MTL_STREAM_OUTPUT_DESC, IMTLD3D11StreamOutputLayout>
      so_layouts;
  dxmt::mutex mutex_so_;

  std::unordered_map<Sha1Digest, std::unique_ptr<CachedSM50Shader>> shaders_;
  std::shared_mutex mutex_shares;

  // gw2 (#6): the key equality is counted (PSO equal=).
  template <typename Pipeline>
  using PipelineMap = std::unordered_map<
      MTL_GRAPHICS_PIPELINE_DESC, std::unique_ptr<Pipeline>, std::hash<MTL_GRAPHICS_PIPELINE_DESC>,
      gw2::PsoKeyEqual>;

  PipelineMap<MTLCompiledGraphicsPipeline> pipelines_;
  dxmt::mutex mutex_;

  PipelineMap<MTLCompiledGeometryPipeline> pipelines_gs_;
  dxmt::mutex mutex_gs_;

  PipelineMap<MTLCompiledTessellationMeshPipeline> pipelines_ts_;
  dxmt::mutex mutex_ts_;

  std::unordered_map<ManagedShader, std::unique_ptr<MTLCompiledComputePipeline>> pipelines_cs_;
  dxmt::mutex mutex_cs_;

  CachedSM50Shader *CreateShader(const void *pBytecode,
                                 uint32_t BytecodeLength) {
    auto sha1 = Sha1HashState::compute(pBytecode, BytecodeLength);
    {
      std::shared_lock<std::shared_mutex> lock(mutex_shares);
      auto result = shaders_.find(sha1);
      if (result != shaders_.end()) {
        return shaders_.at(sha1).get();
      }
    }
    sm50_error_t err;
    sm50_shader_t sm50;
    MTL_SHADER_REFLECTION reflection;
    if (SM50Initialize(pBytecode, BytecodeLength, &sm50, &reflection, &err)) {
      ERR("Failed to initialize shader: ", SM50GetErrorMessageString(err));
      SM50FreeError(err);
      return nullptr;
    }
    auto shader = std::make_unique<CachedSM50Shader>(this, sm50, sha1, reflection);
    {
      std::unique_lock<std::shared_mutex> lock(mutex_shares);
      auto result = shaders_.find(sha1);
      if (result != shaders_.end()) {
        return shaders_.at(sha1).get();
      }
#ifdef DXMT_DEBUG
      shader->bytecode = malloc(BytecodeLength);
      shader->bytecode_length = BytecodeLength;
      memcpy(shader->bytecode, pBytecode, BytecodeLength);
#endif
      return shaders_.emplace(sha1, std::move(shader)).first->second.get();
    }
  }

  virtual HRESULT AddVertexShader(const void *pBytecode,
                                  uint32_t BytecodeLength,
                                  ID3D11VertexShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader =
        ref(new TShaderBase<ID3D11VertexShader, MTLD3D10VertexShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddPixelShader(const void *pBytecode, uint32_t BytecodeLength,
                                 ID3D11PixelShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader = ref(new TShaderBase<ID3D11PixelShader, MTLD3D10PixelShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddHullShader(const void *pBytecode, uint32_t BytecodeLength,
                                ID3D11HullShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader = ref(new TShaderBase<ID3D11HullShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddDomainShader(const void *pBytecode,
                                  uint32_t BytecodeLength,
                                  ID3D11DomainShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader =
        ref(new TShaderBase<ID3D11DomainShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddGeometryShader(const void *pBytecode,
                                    uint32_t BytecodeLength,
                                    ID3D11GeometryShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader =
        ref(new TShaderBase<ID3D11GeometryShader, MTLD3D10GeometryShader>(device, managed_shader));
    return S_OK;
  }

  virtual HRESULT AddComputeShader(const void *pBytecode,
                                   uint32_t BytecodeLength,
                                   ID3D11ComputeShader **ppShader) override {
    auto managed_shader = CreateShader(pBytecode, BytecodeLength);
    if (!managed_shader) {
      return E_FAIL;
    }
    *ppShader =
        ref(new TShaderBase<ID3D11ComputeShader>(device, managed_shader));
    return S_OK;
  }

  HRESULT AddInputLayout(const void *pShaderBytecodeWithInputSignature,
                         const D3D11_INPUT_ELEMENT_DESC *pInputElementDesc,
                         UINT NumElements,
                         IMTLD3D11InputLayout **ppInputLayout) override {
    std::lock_guard<dxmt::mutex> lock(mutex_ia_);
    std::vector<MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC> buffer(NumElements);
    uint32_t num_metal_ia_elements;
    HRESULT hr;
    if (FAILED(hr = ExtractMTLInputLayoutElements(
            device, pShaderBytecodeWithInputSignature, pInputElementDesc,
            NumElements, buffer.data(), &num_metal_ia_elements))) {
      return hr;
    }
    buffer.resize(num_metal_ia_elements);
    if (!input_layouts.contains(buffer)) {
      uint32_t input_slot_mask = 0;
      for (auto &element : buffer) {
        input_slot_mask |= (1 << element.Slot);
      }
      input_layouts.emplace(buffer, std::make_unique<CachedInputLayout>(
                                        std::move(buffer), input_slot_mask));
    }
    *ppInputLayout =
        ref(new MTLD3D11InputLayout(device, input_layouts.at(buffer).get()));
    return hr;
  }

  HRESULT
  AddStreamOutputLayout(const void *pShaderBytecode, UINT NumEntries,
                        const D3D11_SO_DECLARATION_ENTRY *pEntries,
                        UINT NumStrides, const UINT *pStrides,
                        UINT RasterizedStream,
                        IMTLD3D11StreamOutputLayout **ppSOLayout) override {
    std::lock_guard<dxmt::mutex> lock(mutex_so_);
    std::vector<MTL_SHADER_STREAM_OUTPUT_ELEMENT_DESC> buffer(NumEntries * 4);
    std::array<uint32_t, 4> strides = {{}};
    uint32_t num_metal_so_elements;
    if (FAILED(ExtractMTLStreamOutputElements(
            device, pShaderBytecode, NumEntries, pEntries, buffer.data(),
            &num_metal_so_elements))) {
      return E_FAIL;
    }
    buffer.resize(num_metal_so_elements);
    for (unsigned i = 0; i < NumStrides; i++) {
      strides[i] = pStrides[i];
    }
    MTL_STREAM_OUTPUT_DESC desc;
    memcpy(desc.Strides, strides.data(), sizeof(strides));
    desc.Elements = std::move(buffer);
    desc.RasterizedStream = RasterizedStream;
    return so_layouts.CreateStateObject(&desc, ppSOLayout);
  };

  HRESULT AddBlendState(const D3D11_BLEND_DESC1 *pBlendDesc,
                        IMTLD3D11BlendState **ppBlendState) override {
    return blend_states.CreateStateObject(pBlendDesc, ppBlendState);
  }

  void GetGraphicsPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                           MTLCompiledGraphicsPipeline **ppPipeline) override {
    gw2::PsoLookupSample sample; // gw2 (#6)
    std::lock_guard<dxmt::mutex> lock(mutex_);
    sample.locked(pipelines_, *pDesc);

    if (auto iter = pipelines_.find(*pDesc); iter != pipelines_.end()) {
      gw2::compilePipelineLookup(true); // gw2 (#5)
      *ppPipeline = iter->second.get();
      return;
    }
    gw2::compilePipelineLookup(false);
    auto [iter, inserted] = pipelines_.insert({*pDesc, CreateGraphicsPipeline(device, pDesc)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated graphics pipeline");
    } else {
      scheduler_.submit(iter->second.get());
      gw2::psoCount(gw2::PsoCounter::New);
    }
    *ppPipeline = iter->second.get();
  }

  void GetGeometryPipeline(
      MTL_GRAPHICS_PIPELINE_DESC *pDesc,
      MTLCompiledGeometryPipeline **ppPipeline) override {
    gw2::PsoLookupSample sample; // gw2 (#6)
    std::lock_guard<dxmt::mutex> lock(mutex_gs_);
    sample.locked(pipelines_gs_, *pDesc);

    if (auto iter = pipelines_gs_.find(*pDesc); iter != pipelines_gs_.end()) {
      gw2::compilePipelineLookup(true); // gw2 (#5)
      *ppPipeline = iter->second.get();
      return;
    }
    gw2::compilePipelineLookup(false);
    auto [iter, inserted] = pipelines_gs_.insert({*pDesc, CreateGeometryPipeline(device, pDesc)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated geometry pipeline");
    } else {
      scheduler_.submit(iter->second.get());
      gw2::psoCount(gw2::PsoCounter::New);
    }
    *ppPipeline = iter->second.get();
  }

  void GetTessellationPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                   MTLCompiledTessellationMeshPipeline *
                                       *ppPipeline) override {
    gw2::PsoLookupSample sample; // gw2 (#6)
    std::lock_guard<dxmt::mutex> lock(mutex_ts_);
    sample.locked(pipelines_ts_, *pDesc);

    if (auto iter = pipelines_ts_.find(*pDesc); iter != pipelines_ts_.end()) {
      gw2::compilePipelineLookup(true); // gw2 (#5)
      *ppPipeline = iter->second.get();
      return;
    }
    gw2::compilePipelineLookup(false);
    auto [iter, inserted] = pipelines_ts_.insert({*pDesc, CreateTessellationMeshPipeline(device, pDesc)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated tessellation pipeline");
    } else {
      scheduler_.submit(iter->second.get());
      gw2::psoCount(gw2::PsoCounter::New);
    }
    *ppPipeline = iter->second.get();
  }

  void GetComputePipeline(MTL_COMPUTE_PIPELINE_DESC *pDesc,
                                  MTLCompiledComputePipeline **ppPipeline) override {
   std::lock_guard<dxmt::mutex> lock(mutex_cs_);

    if (auto iter = pipelines_cs_.find(pDesc->ComputeShader); iter != pipelines_cs_.end()) {
      gw2::compilePipelineLookup(true); // gw2 (#5)
      *ppPipeline = iter->second.get();
      return;
    }
    gw2::compilePipelineLookup(false);
    auto [iter, inserted] = pipelines_cs_.insert({pDesc->ComputeShader, CreateComputePipeline(device, pDesc->ComputeShader)});
    if (!inserted) {
      D3D11_ASSERT(0 && "duplicated compute pipeline");
    } else {
      scheduler_.submit(iter->second.get());
    }
    *ppPipeline = iter->second.get();
  }

public:
  PipelineCache(MTLD3D11Device *pDevice) :
      scache_(ShaderCache::getInstance(pDevice->GetDXMTDevice().metalVersion())),
      device(pDevice),
      blend_states(pDevice),
      so_layouts(pDevice) {
    // gw2 (#6): the effective pipeline lookup settings, once per device (the log is rewritten
    // per device).
    auto &pso = gw2::psoSettings();
    Logger::info(std::format(
        "PSO: blend hash {} (DXMT_PSO_BLEND_HASH), MRU {} (DXMT_PSO_MRU), topology keep {} (DXMT_PSO_TOPOLOGY_KEEP)",
        pso.blend_hash ? "on" : "off", pso.mru ? "on, 4 entries" : "off", pso.topology_keep ? "on" : "off"
    ));

    // gw2 (#5): DXMT_PSO_ARCHIVE=1 loads earlier sessions' pipeline archives (once per process)
    gw2::psoArchiveInit(pDevice->GetMTLDevice().handle);
  };

  ~PipelineCache() {
    gw2::psoArchiveRequestFlush(); // gw2 (#5): non-blocking
  }
};

std::unique_ptr<MTLD3D11PipelineCacheBase>
InitializePipelineCache(MTLD3D11Device *device) {
  return std::make_unique<PipelineCache>(device);
}

}; // namespace dxmt