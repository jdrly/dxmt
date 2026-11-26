#pragma once

#include "Metal.hpp"
#include <atomic>

namespace dxmt {

class SpatialScaler {
public:
  void incRef();
  void decRef();

  SpatialScaler(WMT::Device device, const WMTFXSpatialScalerInfo &info);
  // gw2 (#56): the Subsample pass's scaler, on Metal 3 or (info.metal4) Metal 4; scaler() is null if
  // MetalFX refused the descriptor
  SpatialScaler(WMT::Device device, WMTGW2SpatialScalerInfo &info);

  bool
  metal4() const {
    return metal4_;
  }

  WMT::FXSpatialScaler scaler() {
    return scaler_;
  }

  WMT::Fence fence() {
    return fence_;
  }

private:
  WMT::Reference<WMT::FXSpatialScaler> scaler_;
  WMT::Reference<WMT::Fence> fence_;
  std::atomic<uint32_t> refcount_;
  bool metal4_ = false; // gw2 (#56)
};

class TemporalScaler {
public:
  void incRef();
  void decRef();

  TemporalScaler(WMT::Device device, const WMTFXTemporalScalerInfo &info);

  WMT::FXTemporalScaler scaler() {
    return scaler_;
  }

  WMT::Fence fence() {
    return fence_;
  }

private:
  WMT::Reference<WMT::FXTemporalScaler> scaler_;
  WMT::Reference<WMT::Fence> fence_;
  std::atomic<uint32_t> refcount_;
};

} // namespace dxmt