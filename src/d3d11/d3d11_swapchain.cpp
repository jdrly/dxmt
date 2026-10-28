#include "d3d11_swapchain.hpp"
#include "com/com_guid.hpp"
#include "config/config.hpp"
#include "d3d11_private.h"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "dxgi_output.hpp"
#include "d3d11_context.hpp"
#include "dxmt_context.hpp"
#include "dxmt_hud_state.hpp"
#include "dxmt_statistics.hpp"
#include "dxmt_gw2_present.hpp"
#include "dxmt_gw2_stats.hpp"
#include "dxmt_presenter.hpp"
#include "log/log.hpp"
#include "d3d11_resource.hpp"
#include "d3d11_device.hpp"
#include "util_cpu_fence.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "util_win32_compat.h"
#include "util_hotpatch.h"
#include "wsi_monitor.hpp"
#include "wsi_platform_win32.hpp"
#include "wsi_window.hpp"
#include "dxmt_info.hpp"
#include "dxmt_presenter.hpp"
#include <atomic>
#include <cfloat>
#include <format>
#include <algorithm>
#include <thread>

/**
Ref: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency
This value is 1 by default.
*/
constexpr size_t kSwapchainLatency = 1;

namespace dxmt {

WMTPixelFormat ConvertSwapChainFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return WMTPixelFormatBGRA8Unorm_sRGB;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return WMTPixelFormatBGRA8Unorm;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTPixelFormatRGB10A2Unorm;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return WMTPixelFormatRGBA16Float;
  default:
    return WMTPixelFormatInvalid;
  }
}

WMTColorSpace ConvertColorSpace(DXGI_COLOR_SPACE_TYPE color_space, bool hdr) {
  switch (color_space) {
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    return WMTColorSpaceSRGB;
  case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    return hdr ? WMTColorSpaceHDR_scRGB : WMTColorSpaceSRGBLinear;
  case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    return WMTColorSpaceHDR_PQ;
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
    return WMTColorSpaceBT2020;
  default:
    return WMTColorSpaceInvalid;
  }
}

/**
 FIXME: duplicated implementation in dxgi_output.cpp
*/
uint32_t
GetMonitorFormatBpp(DXGI_FORMAT Format) {
  switch (Format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    return 32;

  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return 64;

  default:
    Logger::warn(str::format("GetMonitorFormatBpp: Unknown format: ", Format));
    return 32;
  }
}

// gw2 (#1): the background throttle's policy and its 2 s numbers. Process-wide: fed by every
// swapchain's ThrottleBackground() on the game's render thread, drained by the BG section on the
// encode thread.
namespace {

enum class BgPolicy { Focus, Occlusion };

struct BgPolicyConfig {
  BgPolicy policy = BgPolicy::Focus;
  uint32_t fps = 30, fps_minimized = 10;
};

const BgPolicyConfig &
bgConfig() {
  static const BgPolicyConfig config = [] {
    BgPolicyConfig c;
    auto fps = env::getEnvVar("DXMT_BG_FPS");
    if (!fps.empty())
      c.fps = uint32_t(std::strtoul(fps.c_str(), nullptr, 10));
    auto min = env::getEnvVar("DXMT_BG_FPS_MINIMIZED");
    if (!min.empty())
      c.fps_minimized = uint32_t(std::strtoul(min.c_str(), nullptr, 10));
    auto policy = env::getEnvVar("DXMT_BG_POLICY");
    if (policy == "occlusion")
      c.policy = BgPolicy::Occlusion;
    else if (!policy.empty() && policy != "focus")
      Logger::warn(str::format("Present: unknown DXMT_BG_POLICY=", policy, ", using focus"));
    Logger::info(str::format(
        "Present: background throttle policy ", c.policy == BgPolicy::Occlusion ? "occlusion" : "focus",
        " (DXMT_BG_POLICY), ", c.fps, " fps in the background (DXMT_BG_FPS), ", c.fps_minimized,
        " fps minimized (DXMT_BG_FPS_MINIMIZED)"
    ));
    return c;
  }();
  return config;
}

// Why the throttle engaged; the BG section shows the code.
enum class BgReason : uint32_t { None, Minimized, Foreground, NoForeground, Occluded, Miniaturized };

const char *
bgReasonText(BgReason r) {
  switch (r) {
  case BgReason::None:
    return nullptr;
  case BgReason::Minimized:
    return "window minimized";
  // A NULL foreground window (pid 0) counts as another process: the Cmd+Tab case under winemac.
  case BgReason::Foreground:
  case BgReason::NoForeground:
    return "another process in the foreground";
  case BgReason::Occluded:
    return "window not visible (occluded)";
  case BgReason::Miniaturized:
    return "window miniaturized";
  }
  return nullptr;
}

const char *
bgReasonCode(BgReason r) {
  switch (r) {
  case BgReason::None:
    return "none";
  case BgReason::Minimized:
    return "minimized";
  case BgReason::Foreground:
    return "foreground";
  case BgReason::NoForeground:
    return "noforeground";
  case BgReason::Occluded:
    return "occluded";
  case BgReason::Miniaturized:
    return "miniaturized";
  }
  return "none";
}

std::atomic<uint64_t> g_bg_presents{0};    // Presents that went through the throttle
std::atomic<uint64_t> g_bg_throttled{0};   // of them, with the throttle engaged
std::atomic<uint64_t> g_bg_engaged_ns{0};  // time between Presents while engaged
std::atomic<uint64_t> g_bg_sleep_ns{0};    // time slept
std::atomic<uint64_t> g_bg_sleep_max_ns{0};
std::atomic<uint32_t> g_bg_fps{0};         // the rate in effect at the last Present
std::atomic<uint32_t> g_bg_reason{0};      // the last reason it engaged for (BgReason)

std::string
BgSection(double window_ms, uint32_t) {
  uint64_t presents = g_bg_presents.exchange(0, std::memory_order_relaxed);
  uint64_t throttled = g_bg_throttled.exchange(0, std::memory_order_relaxed);
  uint64_t engaged_ns = g_bg_engaged_ns.exchange(0, std::memory_order_relaxed);
  uint64_t sleep_ns = g_bg_sleep_ns.exchange(0, std::memory_order_relaxed);
  uint64_t sleep_max_ns = g_bg_sleep_max_ns.exchange(0, std::memory_order_relaxed);
  uint32_t fps = g_bg_fps.load(std::memory_order_relaxed);
  // Only a window in which the throttle engaged gets a line: it marks that window's STATS as
  // throttled, not game performance.
  if (!throttled && !engaged_ns && !fps)
    return {};
  double share = window_ms > 0 ? std::min(100.0, engaged_ns / 1e6 / window_ms * 100) : 0;
  return std::format(
      "throttled={}/{} share={:.0f}% bgsleep={:.1f}/{:.1f} fps={} reason={} policy={}", throttled, presents, share,
      presents ? sleep_ns / 1e6 / presents : 0.0, sleep_max_ns / 1e6, fps,
      bgReasonCode(BgReason(g_bg_reason.load(std::memory_order_relaxed))),
      bgConfig().policy == BgPolicy::Occlusion ? "occlusion" : "focus"
  );
}

gw2::SectionRegistrar g_bg_section("BG", &BgSection);

} // namespace

class ModeSetGuard {
  std::atomic_flag in_progress_;
public:
  class ModeSetInProgress {
    ModeSetGuard &guard_;
    bool protected_;

  public:
    ModeSetInProgress(ModeSetGuard &guard) : guard_(guard) {
      protected_ = guard_.in_progress_.test_and_set();
    }
    ~ModeSetInProgress() {
      if (!protected_) {
        guard_.in_progress_.clear();
      }
    }
    operator bool() {
      return protected_;
    }
  };
};

template <bool EnableMetalFX>
class MTLD3D11SwapChain final : public MTLDXGISubObject<IDXGISwapChain4, MTLD3D11Device> {
public:
  MTLD3D11SwapChain(
      IDXGIFactory1 *pFactory, MTLD3D11Device *pDevice, IMTLDXGIDevice *pDXGIDevice,
       HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc
  ) :
      MTLDXGISubObject(pDevice),
      factory_(pFactory),
      dxgi_device_(pDXGIDevice),
      presentation_count_(0),
      desc_(*pDesc),
      device_context_(pDevice->GetImmediateContextPrivate()),
      hWnd(hWnd),
      monitor_(wsi::getWindowMonitor(hWnd)),
      hud(WMT::DeveloperHUDProperties::instance()) {

    native_view_ = WMT::CreateMetalViewFromHWND((intptr_t)hWnd, pDevice->GetMTLDevice(), layer_weak_);

    if (!native_view_) {
      ERR("Failed to create metal view, it seems like your Wine has no exported symbols needed by DXMT.");
      abort();
    }

    if constexpr (EnableMetalFX) {
      scale_factor = std::max(Config::getInstance().getOption<float>("d3d11.metalSpatialUpscaleFactor", 2), 1.0f);
    }

    presenter = Rc(new Presenter(pDevice->GetMTLDevice(), layer_weak_,
                                 pDevice->GetDXMTDevice().queue().cmd_library,
                                 scale_factor, desc_.SampleDesc.Count));

    frame_latency = kSwapchainLatency;
    present_semaphore_ = CreateSemaphore(nullptr, frame_latency,
                                         DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);

    // without this flag, there is still a DXGIDevice level of frame latency control
    if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
      frame_latency_fence_ = std::make_unique<CpuFence>();
    }

    if (desc_.Width == 0 || desc_.Height == 0) {
      wsi::getWindowSize(hWnd, &desc_.Width, &desc_.Height);
    }

    if (pFullscreenDesc) {
      fullscreen_desc_ = *pFullscreenDesc;
    } else {
      fullscreen_desc_.Windowed = true;
    }

    preferred_max_frame_rate =
        Config::getInstance().getOption<int>("d3d11.preferredMaxFrameRate", 0);
    wsi::WsiMode current_mode;
    if (wsi::getCurrentDisplayMode(monitor_, &current_mode) &&
        current_mode.refreshRate.denominator != 0 &&
        current_mode.refreshRate.numerator != 0) {
      init_refresh_rate_ = (double)current_mode.refreshRate.numerator /
                           (double)current_mode.refreshRate.denominator;
    }

    handle_alt_tab_ = Config::getInstance().getOption<bool>("dxgi.handleAltTab", false);

    hud.initialize(GetVersionDescriptionText(device_->GetDirectXVersion(), device_->GetFeatureLevel()));

    backbuffer_desc_ = D3D11_TEXTURE2D_DESC1 {
      .Width = desc_.Width,
      .Height = desc_.Height,
      .MipLevels = 1,
      .ArraySize = 1,
      .Format = desc_.Format,
      .SampleDesc = desc_.SampleDesc,
      .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = D3D11_BIND_RENDER_TARGET,
      .CPUAccessFlags = {},
      .MiscFlags = {},
      .TextureLayout = {},
    };
    
    if (desc_.BufferUsage & DXGI_USAGE_SHADER_INPUT)
      backbuffer_desc_.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      backbuffer_desc_.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    // FIXME: check HRESULT!
    ResizeBuffers(0, desc_.Width, desc_.Height, DXGI_FORMAT_UNKNOWN, desc_.Flags);
    if (!fullscreen_desc_.Windowed)
      EnterFullscreenMode(nullptr);
  };

  ~MTLD3D11SwapChain() {
    device_context_->WaitUntilGPUIdle();
    WMT::ReleaseMetalView(native_view_);
    native_view_ = {};
    CloseHandle(present_semaphore_);
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
        riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) ||
        riid == __uuidof(IDXGISwapChain4)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGISwapChain1), riid)) {
      WARN("DXGISwapChain: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetParent(REFIID riid, void **parent) final {
    return factory_->QueryInterface(riid, parent);
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  Present(UINT sync_interval, UINT flags) final {
    return Present1(sync_interval, flags, nullptr);
  };

  HRESULT
  STDMETHODCALLTYPE
  GetBuffer(UINT buffer_idx, REFIID riid, void **surface) final {
    if (buffer_idx == 0) {
      return backbuffer_->QueryInterface(riid, surface);
    } else {
      ERR("Non zero-index buffer is not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) final {
    Com<IDXGIOutput1> target;

    if (pTarget) {
      DXGI_OUTPUT_DESC desc;

      pTarget->QueryInterface(IID_PPV_ARGS(&target));
      target->GetDesc(&desc);

      if (!fullscreen_desc_.Windowed && Fullscreen && monitor_ != desc.Monitor) {
        HRESULT hr = LeaveFullscreenMode();
        if (FAILED(hr))
          return hr;
      }
    }

    if (fullscreen_desc_.Windowed && Fullscreen)
      return EnterFullscreenMode(target.ptr());
    else if (!fullscreen_desc_.Windowed && !Fullscreen)
      return LeaveFullscreenMode();

    return S_OK;
  };

  HRESULT EnterFullscreenMode(IDXGIOutput1* pTarget) {
    ModeSetGuard::ModeSetInProgress modeset_inprogress(modeset_guard_);
    if (modeset_inprogress)
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

    Com<IDXGIOutput1> output = pTarget;

    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    
    if (output == nullptr) {
      if (FAILED(GetOutputFromMonitor(wsi::getWindowMonitor(hWnd), &output))) {
       ERR("DXGI: EnterFullscreenMode: Cannot query containing output");
        return E_FAIL;
      }
    }

    std::unique_lock<dxmt::mutex> lock(mutex_);

    DXGI_MODE_DESC1 preferred_display_mode = {
        desc_.Width,
        desc_.Height,
        fullscreen_desc_.RefreshRate,
        desc_.Format,
        DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
        DXGI_MODE_SCALING_UNSPECIFIED
    };
    if (FAILED(ChangeDisplayMode(output.ptr(), &preferred_display_mode))) {
      ERR("DXGI: EnterFullscreenMode: Failed to change display mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    // Update swap chain description
    fullscreen_desc_.Windowed = FALSE;
    
    // Move the window so that it covers the entire output
    bool modeSwitch = (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0u;

    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);

    monitor_ = desc.Monitor;
    target_  = std::move(output);

    lock = {};

    if (!wsi::enterFullscreenMode(desc.Monitor, hWnd, &window_state_, modeSwitch)) {
      ERR("DXGI: EnterFullscreenMode: Failed to enter fullscreen mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    return S_OK;
  }
  
  
  HRESULT LeaveFullscreenMode() {
    ModeSetGuard::ModeSetInProgress modeset_inprogress(modeset_guard_);
    if (modeset_inprogress)
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

    std::lock_guard<dxmt::mutex> lock(mutex_);

    if (FAILED(RestoreDisplayMode(monitor_)))
      WARN("DXGI: LeaveFullscreenMode: Failed to restore display mode");

    // Restore internal state
    fullscreen_desc_.Windowed = TRUE;
    target_  = nullptr;
    monitor_ = wsi::getWindowMonitor(hWnd);
    
    if (!wsi::isWindow(hWnd))
      return S_OK;
    
    if (!wsi::leaveFullscreenMode(hWnd, &window_state_, true)) {
      ERR("DXGI: LeaveFullscreenMode: Failed to exit fullscreen mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    
    presenter->changeGammaRamp(nullptr);

    return S_OK;
  }

  HRESULT
  ChangeDisplayMode(IDXGIOutput1 *pOutput, DXGI_MODE_DESC1 *pDisplayMode) {
    if (!pOutput)
      return DXGI_ERROR_INVALID_CALL;

    // Find a mode that the output supports

    DXGI_MODE_DESC1 preferred_mode = *pDisplayMode;
    DXGI_MODE_DESC1 selected_mode = {};

    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)) {
      preferred_mode.Width = 0;
      preferred_mode.Height = 0;
    }

    if (preferred_mode.Format == DXGI_FORMAT_UNKNOWN)
      preferred_mode.Format = desc_.Format;

    HRESULT hr = pOutput->FindClosestMatchingMode1(&preferred_mode, &selected_mode, nullptr);

    if (FAILED(hr)) {
      ERR("DXGI: Failed to query closest mode:"
          "\n"
          "  Format: ",
          preferred_mode.Format,
          "\n"
          "  Mode:   ",
          preferred_mode.Width, "x", preferred_mode.Height, "@",
          preferred_mode.RefreshRate.Numerator / std::max(preferred_mode.RefreshRate.Denominator, 1u));
      return hr;
    }

    if (!selected_mode.RefreshRate.Denominator)
      selected_mode.RefreshRate.Denominator = 1;

    DXGI_OUTPUT_DESC output_desc;
    pOutput->GetDesc(&output_desc);
    wsi::WsiMode wsi_mode{
        selected_mode.Width,
        selected_mode.Height,
        {selected_mode.RefreshRate.Numerator, selected_mode.RefreshRate.Denominator},
        GetMonitorFormatBpp(selected_mode.Format),
        selected_mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST ||
            selected_mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST
    };
    if (!wsi::setWindowMode(output_desc.Monitor, hWnd, wsi_mode))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    *pDisplayMode = selected_mode;
    init_refresh_rate_ = double(selected_mode.RefreshRate.Numerator) / double(selected_mode.RefreshRate.Denominator);
    return S_OK;
  }

  HRESULT
  RestoreDisplayMode(HMONITOR hMonitor) {
    if (!hMonitor)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::restoreDisplayMode(hMonitor))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    init_refresh_rate_ = DBL_MAX;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) final {
    HRESULT hr = S_OK;

    if (handle_alt_tab_ && !fullscreen_desc_.Windowed && !wsi::isForeground(hWnd))
      SetFullscreenState(FALSE, nullptr);

    if (pFullscreen != nullptr)
      *pFullscreen = !fullscreen_desc_.Windowed;
    
    if (ppTarget != nullptr)
      *ppTarget = target_.ref();

    return hr;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) final {
    if (!pDesc)
      return E_INVALIDARG;

    pDesc->BufferDesc.Width = desc_.Width;
    pDesc->BufferDesc.Height = desc_.Height;
    pDesc->BufferDesc.RefreshRate = fullscreen_desc_.RefreshRate;
    pDesc->BufferDesc.Format = desc_.Format;
    pDesc->BufferDesc.ScanlineOrdering = fullscreen_desc_.ScanlineOrdering;
    pDesc->BufferDesc.Scaling = fullscreen_desc_.Scaling;
    pDesc->SampleDesc = desc_.SampleDesc;
    pDesc->BufferUsage = desc_.BufferUsage;
    pDesc->BufferCount = desc_.BufferCount;
    pDesc->OutputWindow = hWnd;
    pDesc->Windowed = fullscreen_desc_.Windowed;
    pDesc->SwapEffect = desc_.SwapEffect;
    pDesc->Flags = desc_.Flags;
    return S_OK;
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                UINT flags) final {
    /* BufferCount ignored */
    if (Width == 0 || Height == 0) {
      wsi::getWindowSize(hWnd, &desc_.Width, &desc_.Height);
    } else {
      desc_.Width = Width;
      desc_.Height = Height;
    }
    if (Format != DXGI_FORMAT_UNKNOWN) {
      if (ConvertSwapChainFormat(Format) != WMTPixelFormatInvalid)
        desc_.Format = Format;
    }

    backbuffer_ = nullptr;
    if (desc_.Width == 0 || desc_.Height == 0) {
      backbuffer_desc_.Width = 1;
      backbuffer_desc_.Height = 1;
    } else {
      backbuffer_desc_.Width = desc_.Width;
      backbuffer_desc_.Height = desc_.Height;
    }

    ApplyLayerProps();

    backbuffer_desc_.Format = desc_.Format;

    backbuffer_ = nullptr;
    if (FAILED(dxmt::CreateDeviceTexture2D(
            device_, &backbuffer_desc_, nullptr, reinterpret_cast<ID3D11Texture2D1 **>(&backbuffer_)
        )))
      return E_FAIL;
    // CreateDeviceTexture2D returns public reference, change to private one here
    backbuffer_->AddRefPrivate();
    backbuffer_->Release();

    if constexpr (EnableMetalFX) {
      D3D11_TEXTURE2D_DESC1 upscaled_desc_ = backbuffer_desc_;
      upscaled_desc_.Height *= scale_factor;
      upscaled_desc_.Width *= scale_factor;
      upscaled_backbuffer_ = nullptr;
      if (FAILED(dxmt::CreateDeviceTexture2D(
              device_, &upscaled_desc_, nullptr, reinterpret_cast<ID3D11Texture2D1 **>(&upscaled_backbuffer_)
          )))
        return E_FAIL;

      WMTFXSpatialScalerInfo info;
      info.input_height = desc_.Height;
      info.input_width = desc_.Width;
      info.output_height = desc_.Height * scale_factor;
      info.output_width = desc_.Width * scale_factor;
      info.color_format = backbuffer_->texture()->pixelFormat();
      info.output_format =upscaled_backbuffer_->texture()->pixelFormat();
      metalfx_scaler = new SpatialScaler(device_->GetMTLDevice(), info);
      D3D11_ASSERT(metalfx_scaler && "otherwise metalfx failed to initialize");
    }

    return S_OK;
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  ResizeTarget(const DXGI_MODE_DESC *pDesc) final {
    if (!pDesc)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;

    std::unique_lock<dxmt::mutex> lock(mutex_);

    // Promote display mode
    DXGI_MODE_DESC1 newDisplayMode = {};
    newDisplayMode.Width = pDesc->Width;
    newDisplayMode.Height = pDesc->Height;
    newDisplayMode.RefreshRate = pDesc->RefreshRate;
    newDisplayMode.Format = pDesc->Format;
    newDisplayMode.ScanlineOrdering = pDesc->ScanlineOrdering;
    newDisplayMode.Scaling = pDesc->Scaling;

    // Update the swap chain description
    if (newDisplayMode.RefreshRate.Numerator != 0)
      fullscreen_desc_.RefreshRate = newDisplayMode.RefreshRate;

    fullscreen_desc_.ScanlineOrdering = newDisplayMode.ScanlineOrdering;
    fullscreen_desc_.Scaling = newDisplayMode.Scaling;

    if (fullscreen_desc_.Windowed) {
      lock = {};
      wsi::resizeWindow(hWnd, &window_state_, newDisplayMode.Width,
                        newDisplayMode.Height);
    } else {
      ChangeDisplayMode(target_.ptr(), &newDisplayMode);
      lock = {};
      wsi::updateFullscreenWindow(monitor_, hWnd, false);
    }

    return S_OK;
  };

  void ApplyLayerProps() {
    auto target_color_space =
        ConvertColorSpace(desc_.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                              ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                              : colorspace_,
                          LayerSupportEDR());
    if (presenter->changeLayerProperties(
            ConvertSwapChainFormat(desc_.Format), target_color_space, desc_.Width * scale_factor,
            desc_.Height * scale_factor, desc_.SampleDesc.Count
        ))
      device_context_->WaitUntilGPUIdle();
  };

  HRESULT GetOutputFromMonitor(
          HMONITOR                  Monitor,
          IDXGIOutput1**            ppOutput) {
    if (!ppOutput)
      return DXGI_ERROR_INVALID_CALL;

    Com<IDXGIAdapter> adapter;
    Com<IDXGIOutput> output;

    if (FAILED(dxgi_device_->GetAdapter(&adapter)))
      return E_FAIL;

    for (uint32_t i = 0; SUCCEEDED(adapter->EnumOutputs(i, &output)); i++) {
      DXGI_OUTPUT_DESC outputDesc;
      output->GetDesc(&outputDesc);
      
      if (outputDesc.Monitor == Monitor)
        return output->QueryInterface(IID_PPV_ARGS(ppOutput));
      
      output = nullptr;
    }
    
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetContainingOutput(IDXGIOutput **ppOutput) final {
    InitReturnPtr(ppOutput);
    
    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;
    
    Com<IDXGIOutput1> output;

    if (target_ == nullptr) {
      HRESULT hr = GetOutputFromMonitor(wsi::getWindowMonitor(hWnd), &output);

      if (FAILED(hr))
        return hr;
    } else {
      output = target_;
    }

    *ppOutput = output.ref();
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) final {
    DEBUG("DXGISwapChain::GetFrameStatistics: stub");
    std::unique_lock<d3d11_device_mutex> lock(device_->mutex);
    stats->PresentCount = presentation_count_;
    stats->SyncRefreshCount = presentation_count_;
    stats->PresentRefreshCount = presentation_count_;
    stats->SyncGPUTime = {};
    stats->SyncQPCTime = {};
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetLastPresentCount(UINT *last_present_count) final {
    if (last_present_count == NULL) {
      return E_POINTER;
    }
    std::unique_lock<d3d11_device_mutex> lock(device_->mutex);
    *last_present_count = presentation_count_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) final {
    if (pDesc == NULL) {
      return E_POINTER;
    }
    *pDesc = desc_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) final {
    if (pDesc == NULL) {
      return E_POINTER;
    }
    *pDesc = fullscreen_desc_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetHwnd(HWND *pHwnd) final {
    if (pHwnd == NULL) {
      return E_POINTER;
    }
    *pHwnd = hWnd;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetCoreWindow(REFIID refiid, void **ppUnk) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  };

  class SyncFrameState {
    uint64_t frame_id_ = 0;
    CpuFence *frame_latency_fence_ = nullptr;

  public:
    SyncFrameState() {}
    SyncFrameState(uint64_t frame_id, CpuFence *frame_latency_fence)
        : frame_id_(frame_id), frame_latency_fence_(frame_latency_fence) {}

    SyncFrameState(const SyncFrameState &) = delete;
    SyncFrameState(SyncFrameState &&move) {
      frame_id_ = move.frame_id_;
      frame_latency_fence_ = move.frame_latency_fence_;
      move.frame_latency_fence_ = nullptr;
    };
    ~SyncFrameState() {
      if (frame_latency_fence_) {
        frame_latency_fence_->signal(frame_id_);
        frame_latency_fence_ = nullptr;
      }
    }
  };

  SyncFrameState SyncFrame(uint64_t current_frame_id) {
    if (frame_latency_fence_) {
      if (current_frame_id > frame_latency)
        frame_latency_fence_->wait(current_frame_id - frame_latency);
      return SyncFrameState(current_frame_id, frame_latency_fence_.get());
    }
    return SyncFrameState();
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  Present1(UINT SyncInterval, UINT PresentFlags,
           const DXGI_PRESENT_PARAMETERS *pPresentParameters) final {
    if (SyncInterval > 4)
      return DXGI_ERROR_INVALID_CALL;

    HRESULT hr = S_OK;
    bool window_minimized = wsi::isMinimized(hWnd);
    if ((window_minimized || desc_.Width == 0 || desc_.Height == 0)
        // MSDN: You will not receive DXGI_STATUS_OCCLUDED if you're using a flip model swap chain.
        && desc_.SwapEffect <= DXGI_SWAP_EFFECT_SEQUENTIAL)
      hr = DXGI_STATUS_OCCLUDED;
    bool should_exit_fs = handle_alt_tab_ // At the moment this is still broken for certain games
                          && !fullscreen_desc_.Windowed && !window_minimized && !wsi::isForeground(hWnd);
    if (hr == S_OK && should_exit_fs)
      hr = DXGI_STATUS_OCCLUDED;
    if (PresentFlags & DXGI_PRESENT_TEST)
      return hr;

    RaiseRenderThreadPriority();
    PresentStart();
    ThrottleBackground();
    // gw2 (#7): the pacing of this present; a frame cap that display sync can't express sleeps here
    Pacing pacing = ComputePacing(SyncInterval);
    LimitFrameRate(pacing.limiter);
    int64_t gw2_present_t0 = gw2::nowNs();

    if (should_exit_fs)
      SetFullscreenState(FALSE, nullptr);

    std::unique_lock<d3d11_device_mutex> lock(device_->mutex);

    device_context_->PrepareFlush();
    if (hr == DXGI_STATUS_OCCLUDED) {
      // flush commands without presenting
      device_context_->Commit();
      return hr;
    }

    double vsync_duration = pacing.vsync_duration;
    presenter->changeDisplaySync(pacing.display_sync);

    // gw2: a present without a minimum duration goes out with a deadline one refresh ahead
    // (presentDrawable:atTime:). macOS runs the GPU at the lowest clock that meets the deadline it
    // can infer, and a plain present carries none: GW2 ran 78% of the time at the minimum clock,
    // the CPU waiting for a slow GPU (LASRecomp #203 measured the same). DXMT_PRESENT_DEADLINE=0
    // turns it off; DXMT_PRESENT_DEADLINE=<ms> sets the deadline.
    double present_deadline = 0;
    if (present_deadline_ms_ != 0) {
      double refresh = init_refresh_rate_ != DBL_MAX ? init_refresh_rate_ : 60.0;
      present_deadline = present_deadline_ms_ > 0 ? present_deadline_ms_ / 1000.0 : 1.0 / refresh;
    }
    stats_deadline_ms_ = present_deadline * 1000;
    stats_sync_interval_ = SyncInterval;
    LogPacing(pacing, SyncInterval, present_deadline);

    auto &cmd_queue = device_->GetDXMTDevice().queue();
    auto chunk = cmd_queue.CurrentChunk();
    chunk->signal_frame_latency_fence_ = cmd_queue.CurrentFrameSeq();
    if (target_) {
      auto output = static_cast<MTLDXGIOutput *>(target_.ptr());
      presenter->changeGammaRamp(output->GetGammaRamp());
    }
    if constexpr (EnableMetalFX) {
      chunk->emitcc([
        this, vsync_duration, present_deadline, backbuffer = backbuffer_->texture(),
        sync_state = SyncFrame(++presentation_count_),
        upscaled = upscaled_backbuffer_->texture(),
        scaler = this->metalfx_scaler, state = presenter->synchronizeLayerProperties()
      ](ArgumentEncodingContext &ctx) mutable {
        auto &scaler_info = ctx.currentFrameStatistics().last_scaler_info;
        scaler_info.type = ScalerType::Spatial;
        scaler_info.input_width = backbuffer->width();
        scaler_info.input_height = backbuffer->height();
        scaler_info.output_width = upscaled->width();
        scaler_info.output_height = upscaled->height();
        ctx.upscale(backbuffer, upscaled, scaler);
        ctx.present(upscaled, presenter, vsync_duration, present_deadline, state.metadata);
        ReleaseSemaphore(present_semaphore_, 1, nullptr);
        this->UpdateStatistics(ctx.queue().statistics, ctx.currentFrameId());
      });
    } else {
      chunk->emitcc([
        this, vsync_duration, present_deadline, state = presenter->synchronizeLayerProperties(),
        sync_state = SyncFrame(++presentation_count_),
        backbuffer = backbuffer_->texture()
      ](ArgumentEncodingContext &ctx) mutable {
        ctx.present(backbuffer, presenter, vsync_duration, present_deadline, state.metadata);
        ReleaseSemaphore(present_semaphore_, 1, nullptr);
        this->UpdateStatistics(ctx.queue().statistics, ctx.currentFrameId());
      });
    }
    device_context_->Commit();

    lock.unlock(); // since PresentBoundary() will and should only stall current thread

    if (gw2::statsEnabled())
      gw2::apiAdd(gw2::Api::Present, uint64_t(gw2::nowNs() - gw2_present_t0));

    cmd_queue.PresentBoundary();

    return hr;
  };

  // gw2 (#3): the game's own pacing, HITCH lines and the startup line, at the top of Present on
  // the game's render thread.
  void PresentStart() {
    if (!gw2::statsEnabled())
      return;
    static const double s_hitch_ms = [] {
      auto v = env::getEnvVar("DXMT_HITCH_MS");
      return v.empty() ? 50.0 : std::strtod(v.c_str(), nullptr);
    }();
    gw2::logStartup(str::format(
        "chunks=", device_->GetDXMTDevice().queue().ChunkCount(), " cpus=", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), " refresh=",
        init_refresh_rate_ != DBL_MAX ? init_refresh_rate_ : 0.0
    ));
    gw2::apiCall(gw2::Api::Present);
    double interval = gw2::appPresent();
    auto &cmd_queue = device_->GetDXMTDevice().queue();
    if (s_hitch_ms > 0 && interval >= s_hitch_ms) {
      auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
      auto &f = cmd_queue.CurrentFrameStatistics();
      auto api = gw2::apiFrameOfThisThread();
      std::string cats;
      for (unsigned i = 0; i < gw2::kApiCount; i++)
        if (api.calls[i])
          cats += str::format(" ", gw2::kApiNames[i], "=", std::format("{:.1f}", api.ns[i] / 1e6), "/", api.calls[i]);
      Logger::info(std::format(
          "HITCH frame={} ms={:.1f} commit={:.1f} sync={:.1f}x{} latency={:.1f} bgsleep={}{}", cmd_queue.CurrentFrameSeq(),
          interval, ms(f.commit_interval), ms(f.sync_interval), f.sync_count, ms(f.present_latency_interval),
          bg_fps_ ? "on" : "off", cats.empty() ? std::string(" dxmt=none") : " dxmt(ms/calls):" + cats
      ));
    }
    gw2::apiFrameReset();
  }

  // gw2 (#9): DXMT_RENDER_THREAD_PRIORITY raises the thread that calls Present (GW2's bgfx render
  // thread, which makes every D3D11 call) on its first Present; it ran 22% of its time on
  // Performance cores in Lion's Arch. =1: the macOS QoS class USER_INTERACTIVE, set on this thread
  // through the bridge (pthread_set_qos_class_self_np). =2: Win32 SetThreadPriority(HIGHEST), which
  // Wine's server turns into Mach scheduling policies (precedence, latency/throughput tier 0), not
  // a QoS class. =3: both, SetThreadPriority first: Wine's thread_policy_set likely takes a thread
  // out of its QoS class, so the QoS class goes last. Once per thread, logged; a failure leaves the
  // thread as it was. No restore: the render thread lives as long as the game, and only the thread
  // itself can change its QoS class, which a swapchain destroyed on another thread couldn't do.
  void RaiseRenderThreadPriority() {
    static const uint32_t s_mode = [] {
      auto v = env::getEnvVar("DXMT_RENDER_THREAD_PRIORITY");
      return v.empty() ? 0u : uint32_t(std::strtoul(v.c_str(), nullptr, 10));
    }();
    if (!s_mode)
      return;
    static thread_local bool t_done = false;
    if (t_done)
      return;
    t_done = true;
    DWORD tid = GetCurrentThreadId();
    if (s_mode > 3) {
      Logger::info(str::format("RenderThreadPriority: thread ", tid, ": DXMT_RENDER_THREAD_PRIORITY=", s_mode,
                               " isn't 1, 2 or 3, left unchanged"));
      return;
    }
    if (s_mode & 2) {
      HANDLE self = GetCurrentThread();
      int old_priority = GetThreadPriority(self);
      BOOL ok = SetThreadPriority(self, THREAD_PRIORITY_HIGHEST);
      DWORD err = ok ? 0 : GetLastError();
      Logger::info(str::format(
          "RenderThreadPriority: thread ", tid, " priority ", old_priority, " -> ", GetThreadPriority(self),
          " (SetThreadPriority THREAD_PRIORITY_HIGHEST: ",
          ok ? std::string("ok") : str::format("error ", err, ", left unchanged"), ")"
      ));
    }
    if (s_mode & 1) {
      WMTQoSClass old_qos, new_qos;
      int err = WMTSetThreadQoSClassSelf(WMTQoSClassUserInteractive, 0, &old_qos, &new_qos);
      Logger::info(str::format(
          "RenderThreadPriority: thread ", tid, " QoS ", QoSName(old_qos), " -> ", QoSName(new_qos),
          " (pthread_set_qos_class_self_np user-interactive: ",
          err ? str::format("error ", err, ", left unchanged") : std::string("ok"), ")"
      ));
    }
  }

  static const char *QoSName(WMTQoSClass qos) {
    switch (qos) {
    case WMTQoSClassUnspecified:
      return "unspecified";
    case WMTQoSClassBackground:
      return "background";
    case WMTQoSClassUtility:
      return "utility";
    case WMTQoSClassDefault:
      return "default";
    case WMTQoSClassUserInitiated:
      return "user-initiated";
    case WMTQoSClassUserInteractive:
      return "user-interactive";
    }
    return "other";
  }

  // gw2: how a present is paced. With display sync (CAMetalLayer's vsync, as d9mt fd76b39) a
  // minimum duration of exactly one refresh on top of it makes a frame that is a hair late wait a
  // whole refresh more, and presentation latches at half rate. A limit stays only for longer
  // intervals (SyncInterval > 1, a frame cap below the refresh rate), half a refresh short so it
  // lands on the refresh it aims for. DXMT_DISPLAY_SYNC=0 restores DXMT's own pacing.
  //
  // gw2 (#7): a minimum duration can only express whole refreshes: a 50 fps cap on 60 Hz (20 ms,
  // 1.2 refreshes) was dropped (vsync_duration 0, so 60 fps), and an exact 20 ms would show every
  // frame for two refreshes (30 fps). A cap that isn't a whole number of refreshes is now kept by
  // a frame limiter on the render thread (fixed cadence, like the background throttle) with display
  // sync and the deadline present; frames then alternate between one and two refreshes. Whole
  // numbers (30, 20 fps on 60 Hz) keep the minimum duration. DXMT_CAP_LIMITER=0 restores dropping.
  struct Pacing {
    bool display_sync = false;
    double vsync_duration = 0; // s: presentDrawable:afterMinimumDuration:
    double limiter = 0;        // s: the render thread's frame interval, 0 = none
    const char *mode = "plain";
    uint32_t vsyncs = 0; // refreshes a frame is meant to stay on screen; 0 = no vsync grid
  };

  Pacing ComputePacing(UINT SyncInterval) {
    Pacing p;
    p.display_sync = display_sync_vsync_ && SyncInterval > 0;
    if (p.display_sync) {
      double refresh = init_refresh_rate_ != DBL_MAX ? init_refresh_rate_ : 60.0;
      double interval = SyncInterval / refresh;
      if (preferred_max_frame_rate)
        interval = std::max(interval, 1.0 / preferred_max_frame_rate);
      double refreshes = interval * refresh;
      bool whole = std::abs(refreshes - std::round(refreshes)) < 0.02;
      if (refreshes < 1.005) {
        p.mode = "vsync";
      } else if (whole || !cap_limiter_) {
        p.vsync_duration = interval > 1.5 / refresh ? interval - 0.5 / refresh : 0;
        p.mode = p.vsync_duration > 0 ? "vsync+after" : "vsync (cap dropped)";
      } else {
        p.limiter = interval;
        p.mode = "vsync+limiter";
      }
      double shown = std::max(p.vsync_duration > 0 ? p.vsync_duration + 0.5 / refresh : 0, p.limiter) * refresh;
      p.vsyncs = std::max(1u, uint32_t(std::ceil(shown - 0.02)));
    } else {
      p.vsync_duration =
          std::max(SyncInterval * 1.0 /
                       (preferred_max_frame_rate ? preferred_max_frame_rate
                                                 : init_refresh_rate_),
                   preferred_max_frame_rate ? 1.0 / preferred_max_frame_rate : 0);
      p.mode = p.vsync_duration > 0 ? "after" : "plain";
    }
    return p;
  }

  // gw2 (#7): the frame limiter of ComputePacing. Fixed cadence from the previous deadline, so an
  // oversleep comes back on the next frame; a frame that overran restarts it.
  clock::time_point cap_deadline_{};
  bool cap_limiter_ = env::getEnvVar("DXMT_CAP_LIMITER") != "0";

  void LimitFrameRate(double interval) {
    auto now = clock::now();
    if (interval <= 0 || cap_deadline_ == clock::time_point{}) {
      cap_deadline_ = interval > 0 ? now : clock::time_point{};
      return;
    }
    cap_deadline_ += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(interval));
    if (cap_deadline_ <= now) {
      cap_deadline_ = now;
      return;
    }
    std::this_thread::sleep_until(cap_deadline_);
  }

  // gw2 (#7): the pacing in effect, once and on each change; also hands it to the PRESENT section.
  struct {
    std::string mode;
    UINT interval = ~0u;
    int cap = -1;
    double refresh = -1, duration = -1, limiter = -1, deadline = -1;
  } pacing_logged_;

  void LogPacing(const Pacing &p, UINT SyncInterval, double deadline) {
    double refresh = init_refresh_rate_ != DBL_MAX ? init_refresh_rate_ : 60.0;
    if (gw2::statsEnabled())
      gw2::presentSetPacing(1000.0 / refresh, p.vsyncs, p.limiter * 1000);
    auto &l = pacing_logged_;
    if (l.mode == p.mode && l.interval == SyncInterval && l.cap == preferred_max_frame_rate && l.refresh == refresh &&
        l.duration == p.vsync_duration && l.limiter == p.limiter && l.deadline == deadline)
      return;
    l.mode = p.mode;
    l.interval = SyncInterval;
    l.cap = preferred_max_frame_rate;
    l.refresh = refresh;
    l.duration = p.vsync_duration;
    l.limiter = p.limiter;
    l.deadline = deadline;
    double rate = refresh;
    if (p.vsync_duration > 0)
      rate = p.display_sync ? refresh / p.vsyncs : 1.0 / p.vsync_duration;
    else if (p.limiter > 0)
      rate = 1.0 / p.limiter;
    else if (!p.display_sync)
      rate = 0;
    Logger::info(std::format(
        "Present pacing: mode={} rate={} refresh={:.2f}{} cap={} interval={} duration={:.2f} ms limiter={:.2f} ms "
        "deadline={}",
        p.mode, rate > 0 ? std::format("{:.1f}", rate) : std::string("uncapped"), refresh,
        init_refresh_rate_ != DBL_MAX ? "" : " (assumed)", preferred_max_frame_rate, SyncInterval,
        p.vsync_duration * 1000, p.limiter * 1000,
        p.vsync_duration > 0 ? std::string("- (minimum duration)")
                             : (deadline > 0 ? std::format("{:.1f} ms", deadline * 1000) : std::string("off"))
    ));
  }

  // gw2: the background throttle (d9mt 42b0c47). When the window isn't visible, macOS doesn't make
  // presents wait for vsync, so after a Cmd+Tab the game rendered uncapped in the background and
  // the Mac ran hot. Present sleeps to DXMT_BG_FPS (default 30) while another process's window is
  // in the foreground and to DXMT_BG_FPS_MINIMIZED (default 10) while it is minimized; 0 turns
  // either off. Fixed cadence from the previous deadline; a frame that overran restarts it.
  //
  // gw2 (#1): DXMT_BG_POLICY picks what "in the background" means. focus (default): another
  // process's window, or none, is in the foreground. occlusion (d9mt 42b0c47): the window isn't
  // visible on screen (NSWindow occlusionState) or is miniaturized, whoever has the focus, so a
  // visible GW2 behind another process's small window (Blish HUD) keeps its frame rate. Every
  // transition is logged with its reason; the BG section reports the sleep and the share.
  uint32_t bg_fps_ = 0;
  const char *bg_reason_ = nullptr;
  HWND bg_fg_ = nullptr;
  DWORD bg_fg_pid_ = 0;
  uint64_t bg_vis_ = 0;        // WMTViewVisibility, cached
  int64_t bg_vis_at_ = 0;      // when bg_vis_ was read (gw2::nowNs); 0: never
  int64_t bg_last_ns_ = 0;     // the previous Present through the throttle (for BG)
  bool bg_last_engaged_ = false;
  clock::time_point bg_deadline_{};

  static constexpr int64_t kBgVisibilityPollNs = 100'000'000; // read the cached state at most 10x/s

  void ThrottleBackground() {
    const auto &config = bgConfig();
    uint32_t fps = 0;
    BgReason code = BgReason::None;
    if (wsi::isMinimized(hWnd)) {
      fps = config.fps_minimized;
      code = BgReason::Minimized;
    } else if (config.policy == BgPolicy::Occlusion) {
      int64_t t = gw2::nowNs();
      if (!bg_vis_at_ || t - bg_vis_at_ >= kBgVisibilityPollNs) {
        bg_vis_ = WMT::MetalViewVisibility(native_view_);
        bg_vis_at_ = t;
      }
      // Unknown (before the first main-thread refresh) or not in a window: treated as visible.
      if ((bg_vis_ & WMTViewVisibilityKnown) && !(bg_vis_ & WMTViewVisibilityNoWindow)) {
        if (bg_vis_ & WMTViewVisibilityMiniaturized) {
          fps = config.fps_minimized;
          code = BgReason::Miniaturized;
        } else if (!(bg_vis_ & WMTViewVisibilityVisible)) {
          fps = config.fps;
          code = BgReason::Occluded;
        }
      }
    } else {
      // Another process in front (Cmd+Tab under winemac), not "hWnd isn't the foreground window":
      // GW2's swapchain window isn't the one Wine reports as foreground while the game is in
      // front, which throttled the whole session to 30 fps (a6047fe).
      HWND fg = GetForegroundWindow();
      DWORD fg_pid = 0;
      if (fg)
        GetWindowThreadProcessId(fg, &fg_pid);
      if (fg_pid != GetCurrentProcessId()) {
        fps = config.fps;
        code = fg ? BgReason::Foreground : BgReason::NoForeground;
      }
      bg_fg_ = fg;
      bg_fg_pid_ = fg_pid;
    }
    const char *reason = fps ? bgReasonText(code) : nullptr;
    if (!fps)
      code = BgReason::None;
    auto now = clock::now();
    if (fps != bg_fps_ || reason != bg_reason_) {
      if (fps)
        Logger::info(str::format("Present: ", reason, ", throttling to ", fps, " fps (", BgDetails(code), ")"));
      else
        Logger::info(str::format("Present: presenting normally again (was: ", bg_reason_ ? bg_reason_ : "-", ")"));
      bg_fps_ = fps;
      bg_reason_ = reason;
      bg_deadline_ = now;
    }

    bool stats = gw2::statsEnabled();
    int64_t t0 = 0;
    if (stats) {
      t0 = gw2::nowNs();
      g_bg_presents.fetch_add(1, std::memory_order_relaxed);
      // The interval since the previous Present (its sleep included) ran under that Present's
      // decision: engaged time when it throttled.
      if (bg_last_engaged_ && bg_last_ns_)
        g_bg_engaged_ns.fetch_add(uint64_t(t0 - bg_last_ns_), std::memory_order_relaxed);
      bg_last_ns_ = t0;
      bg_last_engaged_ = fps != 0;
      g_bg_fps.store(fps, std::memory_order_relaxed);
      if (fps) {
        g_bg_throttled.fetch_add(1, std::memory_order_relaxed);
        g_bg_reason.store(uint32_t(code), std::memory_order_relaxed);
      }
    }

    bool slept = false;
    if (fps) {
      bg_deadline_ += std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(1000000000ull / fps));
      if (bg_deadline_ <= now) {
        bg_deadline_ = now;
      } else {
        std::this_thread::sleep_until(bg_deadline_);
        slept = true;
      }
    }

    if (stats && slept) {
      uint64_t ns = uint64_t(gw2::nowNs() - t0);
      g_bg_sleep_ns.fetch_add(ns, std::memory_order_relaxed);
      uint64_t prev = g_bg_sleep_max_ns.load(std::memory_order_relaxed);
      while (ns > prev && !g_bg_sleep_max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed))
        ;
    }
  }

  // gw2 (#1): what the throttle decided on, for its transition line.
  std::string BgDetails(BgReason code) {
    const char *policy = bgConfig().policy == BgPolicy::Occlusion ? "occlusion" : "focus";
    switch (code) {
    case BgReason::Minimized:
      return str::format("policy ", policy, ", IsIconic");
    case BgReason::Foreground:
      return str::format(
          "policy focus, foreground window ", (void *)bg_fg_, " pid ", bg_fg_pid_, ", ours ", GetCurrentProcessId()
      );
    case BgReason::NoForeground:
      return str::format(
          "policy focus, no foreground window (pid 0), counted as another process; ours ", GetCurrentProcessId()
      );
    case BgReason::Occluded:
    case BgReason::Miniaturized:
      return std::format(
          "policy occlusion, visibility 0x{:x}: visible={} miniaturized={} activeSpace={}", bg_vis_,
          (bg_vis_ & WMTViewVisibilityVisible) ? 1 : 0, (bg_vis_ & WMTViewVisibilityMiniaturized) ? 1 : 0,
          (bg_vis_ & WMTViewVisibilityOnActiveSpace) ? 1 : 0
      );
    case BgReason::None:
      break;
    }
    return str::format("policy ", policy);
  }

  // gw2: one STATS line every 2 s from the frame statistics the HUD shows (DXMT_STATS=0 turns it
  // off). Times are ms per frame as avg/max over the window; fps and ms from the encode thread's
  // present-to-present interval.
  struct StatsWindow {
    clock::time_point start{}, last{};
    uint32_t frames = 0, cb = 0, rp = 0, rp_opt = 0, clear = 0, split = 0, sync = 0, stall = 0;
    double frame_max = 0, drawable = 0, drawable_max = 0, commit = 0, commit_max = 0, sync_ms = 0, sync_max = 0;
    double prepare = 0, flush = 0, flush_max = 0, latency = 0, latency_max = 0;
    double next_drawable = 0, next_drawable_max = 0, wait = 0, wait_max = 0;
    uint32_t br_plain = 0, br_deadline = 0, br_after = 0;
  } stats_;
  bool stats_enabled_ = env::getEnvVar("DXMT_STATS") != "0";
  std::atomic<double> stats_deadline_ms_ = 0;
  std::atomic<uint32_t> stats_sync_interval_ = 0;

  void LogStatistics(const FrameStatistics &frame) {
    auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    auto now = clock::now();
    auto &w = stats_;
    if (w.frames == 0 && w.start == clock::time_point{}) {
      w.start = w.last = now;
      return;
    }
    w.frame_max = std::max(w.frame_max, ms(now - w.last));
    w.last = now;
    w.frames++;
    w.cb += frame.command_buffer_count;
    w.rp += frame.render_pass_count;
    w.rp_opt += frame.render_pass_optimized;
    w.clear += frame.clear_pass_count - frame.clear_pass_optimized;
    w.split += frame.present_split_count;
    w.sync += frame.sync_count;
    w.stall += frame.event_stall;
    w.drawable += ms(frame.drawable_blocking_interval);
    w.drawable_max = std::max(w.drawable_max, ms(frame.drawable_blocking_interval));
    w.commit += ms(frame.commit_interval);
    w.commit_max = std::max(w.commit_max, ms(frame.commit_interval));
    w.sync_ms += ms(frame.sync_interval);
    w.sync_max = std::max(w.sync_max, ms(frame.sync_interval));
    w.prepare += ms(frame.encode_prepare_interval);
    w.flush += ms(frame.encode_flush_interval - frame.drawable_blocking_interval);
    w.flush_max = std::max(w.flush_max, ms(frame.encode_flush_interval - frame.drawable_blocking_interval));
    w.latency += ms(frame.present_latency_interval);
    w.latency_max = std::max(w.latency_max, ms(frame.present_latency_interval));
    w.next_drawable += ms(frame.next_drawable_interval);
    w.next_drawable_max = std::max(w.next_drawable_max, ms(frame.next_drawable_interval));
    double wait = ms(frame.commit_interval + frame.sync_interval + frame.present_latency_interval);
    w.wait += wait;
    w.wait_max = std::max(w.wait_max, wait);
    w.br_plain += frame.present_plain;
    w.br_deadline += frame.present_deadline;
    w.br_after += frame.present_after;
    double elapsed = ms(now - w.start);
    if (elapsed < 2000)
      return;
    double n = w.frames;
    Logger::info(std::format(
        "STATS fps={:.1f} ms={:.1f}/{:.1f} cb={:.1f} rp={:.1f}(+{:.1f} merged) clear={:.1f} split={:.2f} "
        "drawable={:.2f}/{:.1f} commit={:.2f}/{:.1f} sync={:.1f}x {:.2f}/{:.1f} stall={} "
        "encode={:.2f}+{:.2f}/{:.1f} latency={:.1f}/{:.1f} maxlatency={} interval={} deadline={:.1f}",
        n * 1000 / elapsed, elapsed / n, w.frame_max, w.cb / n, (w.rp - w.rp_opt) / n, w.rp_opt / n, w.clear / n,
        w.split / n, w.drawable / n, w.drawable_max, w.commit / n, w.commit_max, w.sync / n, w.sync_ms / n,
        w.sync_max, w.stall, w.prepare / n, w.flush / n, w.flush_max, w.latency / n, w.latency_max, frame.latency,
        stats_sync_interval_.load(), stats_deadline_ms_.load()
    ));
    Logger::info(std::format(
        "STATS2 nextdrawable={:.2f}/{:.1f} present=plain:{}/deadline:{}/after:{} wait={:.2f}/{:.1f}", w.next_drawable / n,
        w.next_drawable_max, w.br_plain, w.br_deadline, w.br_after, w.wait / n, w.wait_max
    ));
    LogPace(elapsed);
    LogGpu();
    gw2::emitSections(elapsed, w.frames);
    w = StatsWindow{};
    w.start = w.last = now;
  }

  // gw2 (#3): the game's own Present-to-Present interval on its render thread, and the render
  // thread's time inside DXMT (Draw/Dispatch sampled 1 in 16 and scaled; Map includes its waits).
  void LogPace(double elapsed) {
    auto app = gw2::appDrain();
    auto api = gw2::apiDrain();
    if (app.empty())
      return;
    std::sort(app.begin(), app.end());
    auto pct = [&](double p) { return app[std::min(app.size() - 1, size_t(p * (app.size() - 1) + 0.5))]; };
    uint32_t slow33 = 0, slow50 = 0;
    for (double v : app) {
      slow33 += v > 33.4;
      slow50 += v > 50.0;
    }
    double frames = app.size();
    std::string dxmt;
    double total = 0;
    for (unsigned i = 0; i < gw2::kApiCount; i++) {
      total += api.ns[i] / 1e6;
      dxmt += std::format(" {}={:.2f}/{:.0f}", gw2::kApiNames[i], api.ns[i] / 1e6 / frames, api.calls[i] / frames);
    }
    Logger::info(std::format(
        "PACE app=p50:{:.1f}/p95:{:.1f}/p99:{:.1f}/max:{:.1f} slow33={} slow50={} n={} dxmt={:.2f}ms/frame (sampled 1/16 "
        "draw+dispatch; ms/calls per frame:{})",
        pct(0.50), pct(0.95), pct(0.99), app.back(), slow33, slow50, app.size(), total / frames, dxmt
    ));
  }

  // gw2 (#3): per frame, the union of DXMT's command buffers' GPU execution spans (not the GPU's
  // active time) and the gap between one frame's last GPU end and the next one's first GPU start.
  void LogGpu() {
    auto frames = gw2::gpuSpanDrain();
    if (frames.empty())
      return;
    double span = 0, span_max = 0, gap = 0, gap_max = 0;
    uint32_t gaps = 0, buffers = 0, missing = 0;
    for (size_t i = 0; i < frames.size(); i++) {
      double s = frames[i].span_ns / 1e6;
      span += s;
      span_max = std::max(span_max, s);
      buffers += frames[i].buffers;
      missing += frames[i].missing;
      if (i && frames[i].first_ns && frames[i - 1].last_ns && frames[i].first_ns > frames[i - 1].last_ns) {
        double g = (frames[i].first_ns - frames[i - 1].last_ns) / 1e6;
        gap += g;
        gap_max = std::max(gap_max, g);
        gaps++;
      }
    }
    double n = frames.size();
    Logger::info(std::format(
        "GPU gpuspan={:.2f}/{:.1f} gpugap={:.2f}/{:.1f} frames={} buffers={:.1f} missing={}", span / n, span_max,
        gaps ? gap / gaps : 0.0, gap_max, frames.size(), buffers / n, missing
    ));
  }

  void UpdateStatistics(const FrameStatisticsContainer& statistics, uint64_t frame_id) {
    if (stats_enabled_)
      LogStatistics(statistics.at(frame_id - 1));
    hud.begin();
    auto &frame = statistics.at(frame_id - 1); // show the previous one frame statistics
    auto &average = statistics.average();
    Flags<FeatureCompatibility> flags = frame.compatibility_flags;
    char text[] = "---------------------------";
    if (flags.test(FeatureCompatibility::UnsupportedGeometryDraw)) {
      text[3] = 'G';
    }
    if (flags.test(FeatureCompatibility::UnsupportedTessellationOutputPrimitive)) {
      text[5] = 'T';
      text[6] = 'O';
    }
    if (flags.test(FeatureCompatibility::UnsupportedIndirectTessellationDraw)) {
      text[8] = 'I';
      text[9] = 'T';
    }
    if (flags.test(FeatureCompatibility::UnsupportedGeometryTessellationDraw)) {
      text[11] = 'G';
      text[12] = 'T';
    }
    if (flags.test(FeatureCompatibility::UnsupportedDrawAuto)) {
      text[14] = 'A';
    }
    if (flags.test(FeatureCompatibility::UnsupportedPredication)) {
      text[16] = 'P';
    }
    if (flags.test(FeatureCompatibility::UnsupportedStreamOutputAppending)) {
      text[18] = 'S';
      text[19] = 'A';
    }
    if (flags.test(FeatureCompatibility::UnsupportedMultipleStreamOutput)) {
      text[21] = 'M';
      text[22] = 'S';
    }
    hud.printLine(text);
    hud.printLine(std::format(
        "Commit: {:2} -{:4.1f} -{:4.1f}", std::min(frame.command_buffer_count, 99u),
        std::min(average.commit_interval.count() / 1000000.0, 99.9),
        std::min(statistics.max().commit_interval.count() / 1000000.0, 99.9)
    ));
    hud.printLine(std::format(
        "Sync:   {:2} {:4.1f}  {:2} {:4.1f} {:2}", std::min(frame.sync_count, 99u),
        std::min(average.sync_interval.count() / 1000000.0, 99.9), std::min(statistics.max().event_stall, 99u),
        std::min(average.present_latency_interval.count() / 1000000.0, 99.9), frame.latency
    ));
    hud.printLine(std::format(
        "Encode: {:4.1f}+{:4.1f}+{:4.1f}={:4.1f}", std::min(average.encode_prepare_interval.count() / 1000000.0, 99.9),
        std::min((average.encode_flush_interval - average.drawable_blocking_interval).count() / 1000000.0, 99.9),
        std::min(average.drawable_blocking_interval.count() / 1000000.0, 99.9),
        std::min((average.encode_prepare_interval + average.encode_flush_interval).count() / 1000000.0, 99.9)
    ));
    hud.printLine(std::format(
        "Render:{:3}+{:<3} Clear:{:3}+{:<2}", std::min(frame.render_pass_count - frame.render_pass_optimized, 999u),
        std::min(frame.render_pass_optimized, 999u),
        std::min(frame.clear_pass_count - frame.clear_pass_optimized, 999u), std::min(frame.clear_pass_optimized, 99u)
    ));
    {
      /* scaler info */
      auto &info = frame.last_scaler_info;
      if (info.type == ScalerType::Temporal) {
        auto &info = frame.last_scaler_info;
        hud.printLine(std::format(
            "MetalFX: Temporal {} {}", info.auto_exposure ? "AEXP" : "", info.motion_vector_highres ? "HMV" : ""
        ));
        hud.printLine(std::format(
            "Scale: {:4}x{:4}->{:4}x{:4}", info.input_width, info.input_height, info.output_width, info.output_height
        ));
      }
      if (info.type == ScalerType::Spatial) {
        hud.printLine(std::format(
            "MetalFX: Spatial"
        ));
        hud.printLine(std::format(
            "Scale: {:4}x{:4}->{:4}x{:4}", info.input_width, info.input_height, info.output_width, info.output_height
        ));
      }
    }
    hud.end();
  }

  BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() final { return FALSE; };

  HRESULT
  STDMETHODCALLTYPE
  GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) final {
    InitReturnPtr(ppRestrictToOutput);
    ERR("DXGISwapChain1::GetRestrictToOutput: not implemented");
    return E_NOTIMPL;
  };

  HRESULT
  STDMETHODCALLTYPE
  SetBackgroundColor(const DXGI_RGBA *pColor) final {
    ERR("DXGISwapChain1::SetBackgroundColor: not implemented");
    return E_NOTIMPL;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetBackgroundColor(DXGI_RGBA *pColor) final {
    if (!pColor)
      return E_INVALIDARG;
    // TODO(swapchain): check if native returns transparent or opaque black
    *pColor = {0, 0, 0, 0};
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  SetRotation(DXGI_MODE_ROTATION Rotation) final {
    ERR("DXGISwapChain1::SetRotation: not implemented");
    return E_NOTIMPL;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetRotation(DXGI_MODE_ROTATION *pRotation) final {
    if (!pRotation)
      return E_INVALIDARG;
    *pRotation = DXGI_MODE_ROTATION_IDENTITY;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  SetSourceSize(UINT Width, UINT Height) override {
    ERR("DXGISwapChain2::SetSourceSize: not implemented");
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  GetSourceSize(UINT *pWidth, UINT *pHeight) override {
    if (pWidth)
      *pWidth = desc_.Width;
    if (pHeight)
      *pHeight = desc_.Height;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    if (max_latency == 0 || max_latency > DXGI_MAX_SWAP_CHAIN_BUFFERS) {
      return E_INVALIDARG;
    }
    if (max_latency > frame_latency) {
      ReleaseSemaphore(present_semaphore_, max_latency - frame_latency,
                       nullptr);
    }
    frame_latency = max_latency;

    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    if (max_latency) {
      *max_latency = frame_latency;
    }
    return S_OK;
  };

  HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
      return nullptr;
    }

    HANDLE result = nullptr;
    HANDLE processHandle = GetCurrentProcess();

    if (!DuplicateHandle(processHandle, present_semaphore_, processHandle,
                         &result, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      return nullptr;
    }

    return result;
  };

  HRESULT STDMETHODCALLTYPE
  SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override {
    ERR("DXGISwapChain2::SetMatrixTransform: not implemented");
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override {
    ERR("DXGISwapChain2::GetMatrixTransform: not implemented");
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(
      DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override {
    if (!pColorSpaceSupport)
      return E_INVALIDARG;
    *pColorSpaceSupport = CGColorSpace_checkColorSpaceSupported(ConvertColorSpace(ColorSpace, false))
            ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
            : 0;
    return S_OK;
  };

  UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
    // TODO(swapchain): can be non-zero once sequential swapchain is implemented
    return 0;
  }

  DXMT_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                         DXGI_FORMAT Format, UINT SwapChainFlags,
                         const UINT *pCreationNodeMask,
                         IUnknown *const *ppPresentQueue) override {
    WARN("DXGISwapChain3::ResizeBuffers1: ignoring d3d12 related parameters");
    return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
  }

  HRESULT STDMETHODCALLTYPE
  SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override {
    auto target_color_space = ConvertColorSpace(ColorSpace, LayerSupportEDR());
    if (presenter->changeLayerColorSpace(target_color_space))
      device_context_->WaitUntilGPUIdle();
    colorspace_ = ColorSpace;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type,
                                           UINT Size,
                                           void *pMetaData) override {
                                            return S_OK;
    if (Type == DXGI_HDR_METADATA_TYPE_NONE) {
      presenter->changeHDRMetadata(nullptr);
      return S_OK;
    }
    if (Type == DXGI_HDR_METADATA_TYPE_HDR10) {
      if (Size != sizeof(WMTHDRMetadata))
        return E_INVALIDARG;
      presenter->changeHDRMetadata(reinterpret_cast<const WMTHDRMetadata *>(pMetaData));
      return S_OK;
    }
    return DXGI_ERROR_UNSUPPORTED;
  }

private:
  bool LayerSupportEDR() {
    WMTEDRValue edr_value;
    MetalLayer_getEDRValue(layer_weak_, &edr_value);
    return edr_value.maximum_potential_edr_color_component_value > 1.0f;
  };

  Com<IDXGIFactory1> factory_;
  Com<IMTLDXGIDevice> dxgi_device_;
  WMT::Object native_view_;
  WMT::MetalLayer layer_weak_;
  ULONG presentation_count_;
  DXGI_SWAP_CHAIN_DESC1 desc_;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc_;
  D3D11_TEXTURE2D_DESC1 backbuffer_desc_;
  IMTLD3D11DeviceContext* device_context_;
  Com<D3D11ResourceCommon, false> backbuffer_;
  HANDLE present_semaphore_;
  std::unique_ptr<CpuFence> frame_latency_fence_;
  HWND hWnd;
  HMONITOR monitor_;
  Com<IDXGIOutput1> target_;
  wsi::DXMTWindowState window_state_;
  uint32_t frame_latency;
  DXGI_COLOR_SPACE_TYPE colorspace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  double init_refresh_rate_ = DBL_MAX;
  int preferred_max_frame_rate = 0;
  bool display_sync_vsync_ = env::getEnvVar("DXMT_DISPLAY_SYNC") != "0";
  // -1: one refresh; 0: off; > 0: that many ms
  double present_deadline_ms_ = [] {
    auto v = env::getEnvVar("DXMT_PRESENT_DEADLINE");
    if (v.empty())
      return -1.0;
    try {
      return std::stod(v);
    } catch (...) {
      return -1.0;
    }
  }();
  HUDState hud;
  Rc<Presenter> presenter;
  ModeSetGuard modeset_guard_;
  dxmt::mutex mutex_;

  bool handle_alt_tab_;

  std::conditional<EnableMetalFX, Rc<SpatialScaler>, std::monostate>::type metalfx_scaler;
  std::conditional<EnableMetalFX, Com<D3D11ResourceCommon>, std::monostate>::type upscaled_backbuffer_;
  float scale_factor = 1.0;
};

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D11Device *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
) {
  if (pDesc == NULL)
    return DXGI_ERROR_INVALID_CALL;
  if (ppSwapChain == NULL)
    return DXGI_ERROR_INVALID_CALL;
  InitReturnPtr(ppSwapChain);

  DWORD window_process_id;
  GetWindowThreadProcessId(hWnd, &window_process_id);
  if (GetProcessId(GetCurrentProcess()) != window_process_id) {
    ERR("CreateSwapChain: cross-process swapchain not supported yet");
    return E_FAIL;
  }

  Com<IMTLDXGIDevice> layer_factory;
  if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&layer_factory)))) {
    ERR("CreateSwapChain: failed to get IMTLDXGIDevice");
    return E_FAIL;
  }
  if ((pDesc->SwapEffect != DXGI_SWAP_EFFECT_DISCARD &&
       pDesc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD) &&
      pDesc->BufferCount != 1) {
    WARN("CreateSwapChain: unsupported swap effect ", pDesc->SwapEffect, " with backbuffer size ", pDesc->BufferCount);
  }
  if (env::getEnvVar("DXMT_METALFX_SPATIAL_SWAPCHAIN") == "1") {
    if (pDevice->GetMTLDevice().supportsFXSpatialScaler()) {
      *ppSwapChain = new MTLD3D11SwapChain<true>(
          pFactory, pDevice, layer_factory.ptr(), hWnd, pDesc, pFullscreenDesc
      );
      return S_OK;
    } else {
      WARN("MetalFX spatial scaler is not supported on this device");
    }
  }
  *ppSwapChain = new MTLD3D11SwapChain<false>(
      pFactory, pDevice, layer_factory.ptr(), hWnd, pDesc, pFullscreenDesc
  );
  return S_OK;
};

} // namespace dxmt