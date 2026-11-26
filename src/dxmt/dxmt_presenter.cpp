
#include <algorithm>
#include "Metal.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_gw2_layer_set.hpp"
#include "dxmt_gw2_lifetime.hpp"
#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_gw2_switches.hpp"
#include "util_likely.hpp"
#include "log/log.hpp"


namespace dxmt {

namespace {
// gw2 (#156): how often Present asks the bridge for the layer's screen values, in ms (0: every Present)
sw::Switch<uint32_t> kLayerPropsPollMs{"DXMT_LAYER_PROPS_POLL_MS", 250, sw::legacy::strtoul32};
std::atomic<uint32_t> g_alive = 0; // gw2 (#185)
// gw2 (#226): DXMT_MTL4_LAYER_SET (dxmt_gw2_layer_set.hpp), and the places on the queue that
// presenters hold with `queue`
sw::Switch<std::string> kLayerSet{gw2::kLayerSetSwitch, gw2::kLayerSetDefault, gw2::layerSetValue};
std::atomic<uint32_t> g_queue_sets = 0;
std::atomic_flag g_queue_sets_full_warned = ATOMIC_FLAG_INIT;

// one more place on the queue, unless kLayerSetQueuePlaces are taken
bool
claimQueuePlace() {
  uint32_t held = g_queue_sets.load(std::memory_order_relaxed);
  do {
    if (held >= gw2::kLayerSetQueuePlaces)
      return false;
  } while (!g_queue_sets.compare_exchange_weak(held, held + 1, std::memory_order_relaxed));
  return true;
}
} // namespace

gw2::LayerSet
gw2::mtl4LayerSet() {
  static const LayerSet value = [] {
    auto raw = kLayerSet.requested();
    auto text = kLayerSet.parse(raw);
    if (!raw.empty() && raw != text)
      Logger::warn(str::format("Metal 4: DXMT_MTL4_LAYER_SET=", raw, " is neither buffer nor queue; using buffer"));
    return text == "queue" ? LayerSet::Queue : LayerSet::Buffer;
  }();
  return value;
}

Presenter::Presenter(WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count) :
    device_(device),
    layer_(layer),
    lib_(lib),
    sample_count_(sample_count),
    layer_poll_(int64_t(kLayerPropsPollMs()) * 1'000'000) {
  layer_.getProps(layer_props_);
  layer_props_.device = device;
  layer_props_.opaque = true;
  layer_props_.display_sync_enabled = false;
  layer_props_.framebuffer_only = false; // how strangely setting it true results in worse performance
  layer_props_.contents_scale = layer_props_.contents_scale * scale_factor;
  // gw2 (#157): DXMT_PRESENT_QUEUE=two holds the layer to two drawables; 0 leaves its three
  layer_props_.maximum_drawable_count = gw2::presentQueue() == gw2::PresentQueue::Two ? 2 : 0;
  if (layer_props_.maximum_drawable_count)
    Logger::info("Presenter: maximumDrawableCount 2 (DXMT_PRESENT_QUEUE=two)");

  WMTTextureInfo texture_info;
  texture_info.type = WMTTextureType2D;
  texture_info.pixel_format = WMTPixelFormatRGBA32Float;
  texture_info.usage = WMTTextureUsageShaderRead;
  texture_info.options = WMTResourceStorageModeShared;
  texture_info.width = DXMT_GAMMA_CP_COUNT;
  texture_info.height = 1;
  texture_info.depth = 1;
  texture_info.mipmap_level_count = 1;
  texture_info.sample_count = 1;
  texture_info.array_length = 1;
  gamma_lut_texture_ = device.newTexture(texture_info);
  g_alive.fetch_add(1, std::memory_order_relaxed);
}

Presenter::~Presenter() {
  // gw2 (#226): DXMT_MTL4_LAYER_SET=queue. The last reference goes only after every chunk that
  // presented with this presenter retired (#185), so no committed command buffer still needs the set
  if (mtl4_set_queue_) {
    mtl4_set_queue_.removeResidencySet(WMT::ResidencySet{mtl4_queue_set_.handle});
    g_queue_sets.fetch_sub(1, std::memory_order_relaxed);
  }
  auto alive = g_alive.fetch_sub(1, std::memory_order_relaxed) - 1;
  Logger::info(str::format("Presenter: freed, ", alive, " alive"));
}

void
Presenter::incRef() {
  refcount_.fetch_add(1, std::memory_order_relaxed);
}

void
Presenter::decRef() {
  if (refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1 && gw2::lifetime::presenterFree())
    delete this;
}

uint32_t
Presenter::alive() {
  return g_alive.load(std::memory_order_relaxed);
}

uint32_t
Presenter::layerSetsOnQueue() {
  return g_queue_sets.load(std::memory_order_relaxed);
}

bool
Presenter::changeLayerProperties(
    WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
) {
  bool should_invalidated = colorspace_ != colorspace;
  should_invalidated |= source_format_ != format;
  source_format_ = format;
  should_invalidated |= sample_count_ != sample_count;
  sample_count_ = sample_count;
  layer_props_.pixel_format = Forget_sRGB(format);
  layer_props_.drawable_height = height;
  layer_props_.drawable_width = width;
  colorspace_ = colorspace;
  layer_poll_.invalidate(); // gw2 (#156): a resize asks the bridge again at the next Present
  if (should_invalidated)
    pso_valid.clear(); // defer changes
  else
    layer_.setProps(layer_props_);
  return should_invalidated;
}

bool
Presenter::changeLayerColorSpace(WMTColorSpace colorspace) {
  bool should_invalidated = colorspace_ != colorspace;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear();
  return should_invalidated;
}

void
Presenter::changeHDRMetadata(const WMTHDRMetadata *metadata) {
  if (metadata) {
    has_hdr_metadata_ = true;
    hdr_metadata_ = *metadata;
    pso_valid.clear();
  } else {
    if (has_hdr_metadata_) {
      has_hdr_metadata_ = false;
      pso_valid.clear();
    }
  }
}

void
Presenter::changeGammaRamp(const DXMTGammaRamp *gamma_ramp) {
  if (!gamma_ramp && gamma_version_ != 0) {
    gamma_version_ = 0;
    pso_valid.clear();
    return;
  }
  if (gamma_ramp && gamma_version_ != gamma_ramp->version) {
    gamma_version_ = gamma_ramp->version;
    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_lut_rgba_[i * 4 + 0] = std::clamp(gamma_ramp->red[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 1] = std::clamp(gamma_ramp->green[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 2] = std::clamp(gamma_ramp->blue[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 3] = 1.0f;
    }
    gamma_lut_texture_.replaceRegion(
          {0, 0, 0}, {DXMT_GAMMA_CP_COUNT, 1, 1}, 0, 0,
          gamma_lut_rgba_.data(),
          DXMT_GAMMA_CP_COUNT * sizeof(float) * 4,
          0);
    pso_valid.clear();
  }
}

void
Presenter::changeDisplaySync(bool enabled) {
  if (layer_props_.display_sync_enabled == enabled)
    return;
  layer_props_.display_sync_enabled = enabled;
  layer_.setProps(layer_props_);
  Logger::info(str::format("Presenter: display sync ", enabled ? "on" : "off"));
}

Presenter::PresentState
Presenter::synchronizeLayerProperties() {
  // gw2 (#156): the bridge call walks the layer's view, window and screen and builds the screen's
  // device description on every call; between asks the last answer stands (DXMT_LAYER_PROPS_POLL_MS)
  if (layer_poll_.due(gw2::nowNs())) {
    uint64_t display_setting_version = 0;
    auto last_colorspace = display_colorspace_;
    auto last_edr = display_edr_value_;

    WMTQueryDisplaySettingForLayer(
        layer_.handle, &display_setting_version, &display_colorspace_, &display_hdr_metadata_, &display_edr_value_
    );

    bool changed = display_setting_version != display_setting_version_ || display_colorspace_ != last_colorspace ||
                   display_edr_value_.maximum_edr_color_component_value != last_edr.maximum_edr_color_component_value ||
                   display_edr_value_.maximum_potential_edr_color_component_value !=
                       last_edr.maximum_potential_edr_color_component_value;
    if (changed)
      Logger::info(str::format(
          "Presenter: display setting version=", display_setting_version, " colorspace=", (int)display_colorspace_,
          " edr=", display_edr_value_.maximum_edr_color_component_value,
          " potential=", display_edr_value_.maximum_potential_edr_color_component_value
      ));

    if (display_setting_version != display_setting_version_) {
      display_setting_version_ = display_setting_version;
      pso_valid.clear();
    }
  }

  auto final_colorspace = display_setting_version_ > 0 ? display_colorspace_ : colorspace_;
  auto is_hdr = WMT_COLORSPACE_IS_HDR(final_colorspace);
  auto hdr_metadata = display_setting_version_ > 0 ? &display_hdr_metadata_
                      : has_hdr_metadata_          ? &hdr_metadata_
                                                   : nullptr;
  if (unlikely(!pso_valid.test_and_set())) {
    frame_presented_.wait(frame_requested_);
    buildRenderPipelineState(final_colorspace == WMTColorSpaceHDR_PQ, is_hdr && hdr_metadata != nullptr, sample_count_ > 1, gamma_version_ != 0);
    layer_.setProps(layer_props_);
    layer_.setColorSpace(final_colorspace);
  }

  DXMTPresentMetadata metadata;

  metadata.edr_scale = 1.0;
  metadata.max_content_luminance = 10000;
  metadata.max_display_luminance = display_edr_value_.maximum_potential_edr_color_component_value * 100;

  if (is_hdr) {
    metadata.edr_scale = display_edr_value_.maximum_edr_color_component_value /
                display_edr_value_.maximum_potential_edr_color_component_value;

    if (hdr_metadata) {
      metadata.max_content_luminance = std::max<uint32_t>(
          {(uint32_t)metadata.max_display_luminance, hdr_metadata->max_content_light_level,
           hdr_metadata->max_mastering_luminance, hdr_metadata->max_frame_average_light_level}
      );
    }
  }

  if (final_colorspace == WMTColorSpaceHDR_scRGB)
    metadata.edr_scale *= 0.8;

  return {metadata, ++frame_requested_, this};
}

WMT::MetalDrawable
Presenter::encodeCommands(
    WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
    std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
    std::function<void(WMT::RenderCommandEncoder)> &&update_fences
) {
  // gw2 (#3): nextDrawable alone for STATS2, only with DXMT_STATS on
  bool stats = gw2::statsEnabled();
  std::chrono::high_resolution_clock::time_point t0;
  if (stats)
    t0 = std::chrono::high_resolution_clock::now();
  auto drawable = layer_.nextDrawable();
  if (stats)
    last_next_drawable_ = std::chrono::high_resolution_clock::now() - t0;

  WMTRenderPassInfo info;
  WMT::InitializeRenderPassInfo(info);
  info.colors[0].load_action = WMTLoadActionDontCare;
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].texture = drawable.texture();
  auto encoder = cmdbuf.renderCommandEncoder(info);
  wait_fences(encoder);
  encoder.setFragmentTexture(backbuffer, 0);
  encoder.setFragmentTexture(gamma_lut_texture_, 1);

  double width = layer_props_.drawable_width;
  double height = layer_props_.drawable_height;

  encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
  if (backbuffer.width() == (uint64_t)width && backbuffer.height() == (uint64_t)height) {
    encoder.setRenderPipelineState(present_blit_);
  } else {
    encoder.setRenderPipelineState(present_scale_);
  }
  encoder.setViewport({0, 0, width, height, 0, 1});
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  update_fences(encoder);
  encoder.endEncoding();

  return drawable;
}

WMT::MetalDrawable
Presenter::encodeCommandsMetal4(
    gw2::Mtl4Encoding &encoding, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
    std::function<void(WMT::MTL4RenderCommandEncoder)> &&wait_fences,
    std::function<void(WMT::MTL4RenderCommandEncoder)> &&update_fences
) {
  if (!mtl4_layer_set_known_) {
    // the drawables' residency: the layer's own set (a driver-owned set would pin them, d9mt)
    mtl4_layer_set_known_ = true;
    mtl4_layer_set_ = layer_.residencySet();
    if (!mtl4_layer_set_)
      WARN("Metal 4: the layer has no residency set; drawables may not be resident");
    else if (gw2::mtl4LayerSet() == gw2::LayerSet::Queue) {
      // gw2 (#226): as before #184, on the queue for the presenter's life. The set is retained while
      // it is there: the swap chain lets go of the layer before the presenter
      if (claimQueuePlace()) {
        mtl4_queue_set_ = mtl4_layer_set_;
        mtl4_set_queue_ = encoding.queue().queue();
        mtl4_set_queue_.addResidencySet(WMT::ResidencySet{mtl4_queue_set_.handle});
      } else if (!g_queue_sets_full_warned.test_and_set()) {
        Logger::warn(str::format(
            "Metal 4: ", gw2::kLayerSetQueuePlaces, " layers' residency sets are on the queue already "
            "(DXMT_MTL4_LAYER_SET=queue); the present passes of later swap chains use their layer's set"
        ));
      }
    }
  }
  // gw2 (#3): nextDrawable alone for STATS2, only with DXMT_STATS on
  bool stats = gw2::statsEnabled();
  std::chrono::high_resolution_clock::time_point t0;
  if (stats)
    t0 = std::chrono::high_resolution_clock::now();
  auto drawable = layer_.nextDrawable();
  if (stats)
    last_next_drawable_ = std::chrono::high_resolution_clock::now() - t0;
  encoding.waitForDrawable(drawable);

  WMTRenderPassInfo info;
  WMT::InitializeRenderPassInfo(info);
  info.colors[0].load_action = WMTLoadActionDontCare;
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].texture = drawable.texture();
  // gw2 (#184): the layer's set on the present pass's command buffer (the only one that touches the
  // drawable), not on the queue: a queue takes at most 32 sets, and a program may make any number of
  // swap chains. gw2 (#226): not with DXMT_MTL4_LAYER_SET=queue and a place on the queue
  if (mtl4_layer_set_ && !mtl4_set_queue_)
    encoding.open().cmdbuf.useResidencySet(WMT::ResidencySet{mtl4_layer_set_.handle});
  auto encoder = encoding.renderCommandEncoder(info, WMT_MTL4_NO_ATTACHMENT_RESIDENCY);
  wait_fences(encoder);
  encoder.setFragmentTexture(backbuffer, 0);
  encoder.setFragmentTexture(gamma_lut_texture_, 1);

  double width = layer_props_.drawable_width;
  double height = layer_props_.drawable_height;

  encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
  if (backbuffer.width() == (uint64_t)width && backbuffer.height() == (uint64_t)height) {
    encoder.setRenderPipelineState(present_blit_);
  } else {
    encoder.setRenderPipelineState(present_scale_);
  }
  encoder.setViewport({0, 0, width, height, 0, 1});
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  update_fences(encoder);
  encoder.endEncoding();

  return drawable;
}

void
Presenter::gw2LifetimeUse(uint32_t queue, uint64_t seq) const {
  using gw2::lifetime::Kind;
  gw2::lifetime::use(queue, seq, present_blit_, Kind::Pipeline);
  gw2::lifetime::use(queue, seq, present_scale_, Kind::Pipeline);
  gw2::lifetime::use(queue, seq, gamma_lut_texture_, Kind::Texture);
}

void
Presenter::buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable) {
  auto pool = WMT::MakeAutoreleasePool();

  auto library = lib_.getLibrary();

  uint32_t true_data = true, false_data = false;
  WMTFunctionConstant constants[6];
  constants[0].data.set(&true_data);
  constants[0].type = WMTDataTypeBool;
  constants[0].index = kPresentFCIndex_BackbufferSizeMatched;
  constants[1].data.set(is_pq ? &true_data : &false_data);
  constants[1].type = WMTDataTypeBool;
  constants[1].index = kPresentFCIndex_HDRPQ;
  constants[2].data.set(with_hdr_metadata ? &true_data : &false_data);
  constants[2].type = WMTDataTypeBool;
  constants[2].index = kPresentFCIndex_WithHDRMetadata;
  constants[3].data.set(Is_sRGBVariant(source_format_) ? &true_data : &false_data);
  constants[3].type = WMTDataTypeBool;
  constants[3].index = kPresentFCIndex_BackbufferIsSRGB;
  constants[4].data.set(is_ms ? &true_data : &false_data);
  constants[4].type = WMTDataTypeBool;
  constants[4].index = kPresentFCIndex_BackbufferIsMS;
  constants[5].data.set(gamma_enable ? &true_data : &false_data);
  constants[5].type  = WMTDataTypeBool;
  constants[5].index = kPresentFCIndex_GammaEnabled;

  WMT::Reference<WMT::Error> error;
  auto vs_present_quad = library.newFunction("vs_present_quad");
  auto fs_present_quad = library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);

  constants[0].data.set(&false_data);
  auto fs_present_quad_scaled =
      library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);
  {
    WMTRenderPipelineInfo present_pipeline;
    WMT::InitializeRenderPipelineInfo(present_pipeline);
    present_pipeline.colors[0].pixel_format = layer_props_.pixel_format;
    present_pipeline.vertex_function = vs_present_quad;
    present_pipeline.fragment_function = fs_present_quad;
    present_blit_ = device_.newRenderPipelineState(present_pipeline, error);
    present_pipeline.fragment_function = fs_present_quad_scaled;
    present_scale_ = device_.newRenderPipelineState(present_pipeline, error);
  }
}

} // namespace dxmt