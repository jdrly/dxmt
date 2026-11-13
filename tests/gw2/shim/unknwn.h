#pragma once
// gw2 (#247): the host-side stand-in for what src/util/com/com_pointer.hpp takes from <unknwn.h>, so a
// native test can build Com without the PE toolchain. Only the names its templates use; nothing here
// is ever called.
typedef long HRESULT;
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
struct GUID {};
template <typename T>
const GUID &
gw2_uuidof() {
  static GUID id;
  return id;
}
#define __uuidof(T) gw2_uuidof<T>()
struct IUnknown {
  HRESULT QueryInterface(const GUID &, void **) { return -1; }
};
