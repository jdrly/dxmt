#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "winemetal.h"
#include <mutex>
#include <utility>

namespace dxmt {
Logger Logger::s_instance("dxgi.log");

VendorExtension g_extension_enabled = VendorExtension::None;

#ifdef _WIN32
std::once_flag nvext_init;

static void InitializeVendorExtensionNV() {
#ifdef __i386__
  return;
#endif
  HKEY key1 = nullptr, key2 = nullptr, key3 = nullptr;
  auto name1 = L"{41FCC608-8496-4DEF-B43E-7D9BD675A6FF}";
  auto name2 = L"FullPath";
  WCHAR value3[] = L"C:\\Windows\\System32";
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\NVIDIA Corporation\\Global", 0, nullptr, 0,
                      KEY_ALL_ACCESS, nullptr, &key1, nullptr) ||
      RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\ControlSet001\\Services\\nvlddmkm", 0, nullptr,
                      0, KEY_ALL_ACCESS, nullptr, &key2, nullptr) ||
      RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
                      nullptr, 0, KEY_ALL_ACCESS, nullptr, &key3, nullptr)) {
    goto cleanup;
  }
  if (env::getEnvVar("DXMT_ENABLE_NVEXT") != "1") {
    RegDeleteValueW(key1, name1);
    RegDeleteValueW(key2, name1);
    RegDeleteValueW(key3, name2);
  } else {
    DWORD _1 = 1;
    RegSetValueExW(key1, name1, 0, REG_DWORD, (const BYTE *)&_1, sizeof(_1));
    RegSetValueExW(key2, name1, 0, REG_DWORD, (const BYTE *)&_1, sizeof(_1));
    RegSetValueExW(key3, name2, 0, REG_SZ, (const BYTE *)value3, sizeof(value3));
    Logger::info("Vendor extension enabled: NVEXT");
    g_extension_enabled = VendorExtension::Nvidia;
  }
  cleanup:
  if (key1) RegCloseKey(key1);
  if (key2) RegCloseKey(key2);
  if (key3) RegCloseKey(key3);
};

static void InitializeMetalCachePath() {
  if (env::getEnvVar("DXMT_USE_DEFAULT_METAL_CACHE") == "1")
    return;
  // This is the framework cache (mainly for PSOs), not managed by DXMT
  auto metal_cache_path = str::format("dxmt/", env::getExeName(), "/com.apple.metal");
  if (!WMTSetMetalShaderCachePath(metal_cache_path.c_str())) {
    // gw2 (#46): Metal fixes its shader cache path at its first device creation, and Wine's display
    // code (winemac's GPU enumeration) creates Metal devices in every GUI process before dxgi.dll
    // loads. So the set can't take; say where Metal's own (persistent) cache is instead.
    char actual[1024];
    uint32_t len = WMTGetMetalShaderCachePath(actual, sizeof(actual));
    Logger::info(str::format(
        "Metal shader cache: ", len ? actual : "(unknown)",
        len >= sizeof(actual) ? "..." : "",
        " (Metal fixed it before DXMT loaded: Wine creates Metal devices first, so dxmt/",
        env::getExeName(), "/com.apple.metal is unused; DXMT_USE_DEFAULT_METAL_CACHE=1 skips the attempt)"
    ));
  }
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason,
                               LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);

  InitializeMetalCachePath();
  std::call_once(nvext_init, InitializeVendorExtensionNV);

  return TRUE;
}

#endif

extern "C" HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid,
                                                    void **ppDebug) {
#ifdef _WIN32
  // it's a DXMT implementation detail
  if (riid == DXMT_NVEXT_GUID) {
    std::call_once(nvext_init, InitializeVendorExtensionNV);
    return g_extension_enabled == VendorExtension::Nvidia ? S_OK : E_NOINTERFACE;
  }
#endif

  return E_NOINTERFACE;
}

extern "C" HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
  static bool enabled = false;

  if (std::exchange(enabled, true))
    return DXGI_ERROR_ALREADY_EXISTS;

  Logger::warn("DXGIDeclareAdapterRemovalSupport: stub");
  return S_OK;
}

} // namespace dxmt