#pragma once

// gw2 (#226): DXMT_MTL4_LAYER_SET, where a Metal 4 presenter names its layer's residency set. A test
// switch for #223 (a moving cursor makes every frame a refresh late on Metal 4), off by default.
//
// - buffer (the default, #184): the present pass's command buffer uses the set; the queue holds no
//   layer's set.
// - queue: the placement before #184 (e02a4d6). A presenter's first Metal 4 present adds the set to
//   the queue, and no command buffer names it. The presenter takes it off the queue when it is
//   deleted (DXMT_PRESENTER_FREE=1); with presenters kept (the default) it stays, as it did then.
//   At most kLayerSetQueuePlaces presenters of a process hold a place, so a process with many swap
//   chains stays under a queue's 32 sets; the others use buffer.
//
// The switch is declared and read once per process in dxmt_presenter.cpp. Its name and grammar are
// here for tests/gw2/switches_test.cpp.

#include <cstdint>
#include <string>

namespace dxmt::gw2 {

enum class LayerSet : uint8_t { Buffer, Queue };

constexpr const char *kLayerSetSwitch = "DXMT_MTL4_LAYER_SET";
constexpr const char *kLayerSetDefault = "buffer";
constexpr uint32_t kLayerSetQueuePlaces = 16;

// The grammar: "queue" is queue; unset, empty, "buffer" and anything else is the fallback, buffer.
inline std::string
layerSetValue(const std::string &raw, const std::string &fallback) {
  return raw == "queue" ? raw : fallback;
}

// DXMT_MTL4_LAYER_SET, read once per process (dxmt_presenter.cpp). Text that is neither value gets
// one warning.
LayerSet mtl4LayerSet();

} // namespace dxmt::gw2
