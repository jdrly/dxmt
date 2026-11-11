#pragma once
#include "Metal.hpp"
#include "dxmt_command.hpp"
#include "dxmt_gw2_layer_poll.hpp"
#include "dxmt_gw2_mtl4_encoding.hpp"
#include "util_cpu_fence.hpp"
#include "winemetal.h"
#include <atomic>
#include <chrono>

namespace dxmt {

struct DXMTPresentMetadata {
  float edr_scale;
  float max_content_luminance;
  float max_display_luminance;
};

constexpr uint32_t DXMT_GAMMA_CP_COUNT = 1024;

struct DXMTGammaRamp {
  float red[DXMT_GAMMA_CP_COUNT];
  float green[DXMT_GAMMA_CP_COUNT];
  float blue[DXMT_GAMMA_CP_COUNT];
  uint64_t version;
};

class Presenter {
public:
  Presenter(
      WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count
  );
  ~Presenter();
  Presenter(const Presenter &) = delete;
  Presenter &operator=(const Presenter &) = delete;

  // gw2 (#185): Rc's count. With DXMT_PRESENTER_FREE=1 the last reference deletes the presenter: the
  // swap chain's, after its destructor waited for every chunk to retire, or the retirement ledger's,
  // once the chunk that presented with it retired. With 0 (the default) it stays at zero, as upstream's
  // RcObject left every presenter.
  void incRef();
  void decRef();

  // gw2 (#185): presenters made and not deleted in this process
  static uint32_t alive();

  bool changeLayerProperties(
      WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
  );

  bool changeLayerColorSpace(WMTColorSpace colorspace);

  void changeHDRMetadata(const WMTHDRMetadata *metadata);

  void changeGammaRamp(const DXMTGammaRamp *gamma_ramp);

  // gw2: Metal's own vsync (CAMetalLayer displaySyncEnabled) for presents with a sync interval
  void changeDisplaySync(bool enabled);

  class PresentState {
  public:
    DXMTPresentMetadata metadata;
    uint64_t frame_id;
    Presenter *presenter;
    PresentState(DXMTPresentMetadata metadata, uint64_t frame_id, Presenter *presenter) :
        metadata(metadata),
        frame_id(frame_id),
        presenter(presenter) {}
    PresentState(const PresentState &copy) = delete;
    PresentState(PresentState &&move)  {
      metadata = move.metadata;
      frame_id = move.frame_id;
      presenter = move.presenter;
      move.presenter = nullptr;
    };
    ~PresentState() {
      if (presenter) {
        presenter->frame_presented_.signal(frame_id);
        presenter = nullptr;
      }
    };
  };

  PresentState synchronizeLayerProperties();

  // gw2 (#31): DXMT_LIFETIME_CHECK: chunk `seq` of `queue` uses the present pass's pipelines and
  // gamma table (they are rebuilt only after every earlier present's chunk retired)
  void gw2LifetimeUse(uint32_t queue, uint64_t seq) const;

  // gw2: how long the last nextDrawable took (#3)
  std::chrono::high_resolution_clock::duration lastNextDrawable() const { return last_next_drawable_; }

  WMT::MetalDrawable encodeCommands(
      WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
      std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
      std::function<void(WMT::RenderCommandEncoder)> &&update_fences
  );

  // gw2 (#32): the present pass on Metal 4. nextDrawable, then the queue waits for the drawable (the
  // command buffers committed after it), then the pass into a command buffer of the encoding. The
  // drawable's texture stays out of the chunk's residency set: the layer's own set covers it, used by
  // the present pass's command buffer (#184). The caller commits, signals the drawable and presents it.
  WMT::MetalDrawable encodeCommandsMetal4(
      gw2::Mtl4Encoding &encoding, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
      std::function<void(WMT::MTL4RenderCommandEncoder)> &&wait_fences,
      std::function<void(WMT::MTL4RenderCommandEncoder)> &&update_fences
  );

private:
  void buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable);

  // gw2 (#32, #184): the layer's residency set, read at the first Metal 4 present (null if it has
  // none). Not retained: the layer owns it, and the swap chain keeps the layer until the GPU is idle
  WMT::Object mtl4_layer_set_;
  bool mtl4_layer_set_known_ = false;

  std::chrono::high_resolution_clock::duration last_next_drawable_{};
  WMT::Device device_;
  WMT::MetalLayer layer_;
  InternalCommandLibrary &lib_;
  WMTLayerProps layer_props_;
  uint32_t sample_count_;
  WMTColorSpace colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata hdr_metadata_;
  bool has_hdr_metadata_ = false;
  WMTPixelFormat source_format_ = WMTPixelFormatInvalid;
  uint64_t display_setting_version_ = 0;
  WMTColorSpace display_colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata display_hdr_metadata_;
  WMTEDRValue display_edr_value_{0.0, 1.0};
  // gw2 (#156): when synchronizeLayerProperties asks the bridge for the values above again
  gw2::LayerPropsPoll layer_poll_;
  uint64_t gamma_version_ = 0;
  std::array<float, DXMT_GAMMA_CP_COUNT * 4> gamma_lut_rgba_;
  WMT::Reference<WMT::Texture> gamma_lut_texture_;
  WMT::Reference<WMT::RenderPipelineState> present_blit_;
  WMT::Reference<WMT::RenderPipelineState> present_scale_;
  std::atomic_flag pso_valid = 0;
  uint64_t frame_requested_ = 0;
  CpuFence frame_presented_ = 0;
  std::atomic<uint32_t> refcount_ = 0;
};
} // namespace dxmt