/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include "thread.hpp"
#ifdef _WIN32
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <atomic>

namespace dxmt {

// gw2 (#137): GW2 frees d3d11.dll at exit while the recipe writer sleeps in it; the writer then
// returned into unmapped code (d3d11.dll+0xAA1AB in 9eb7668, the return from its Sleep). Joining in
// DLL_PROCESS_DETACH can't work: it runs under the loader lock, which an exiting thread needs
// (LdrShutdownThread), and the process-wide threads have no owner to stop them. So the module stays
// mapped once it has a thread; FreeLibrary then only drops a reference, and the threads end with the
// process, as when a game never unloads d3d11.dll.
void
pinModuleForThreads() {
  static std::atomic<bool> done{false};
  if (done.exchange(true, std::memory_order_acq_rel))
    return;
  if (env::getEnvVar("DXMT_PIN_MODULE") == "0") {
    Logger::info("Lifetime: not pinned (DXMT_PIN_MODULE=0); FreeLibrary can unmap code its threads still run");
    return;
  }
  HMODULE module = nullptr;
  if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&pinModuleForThreads), &module))
    Logger::info("Lifetime: pinned for the process before its first thread; FreeLibrary no longer unloads it "
                 "(DXMT_PIN_MODULE)");
  else
    Logger::warn(str::format("Lifetime: can't pin the module (error ", ::GetLastError(),
                             "); its threads may outlive FreeLibrary (DXMT_PIN_MODULE)"));
}

} // namespace dxmt

namespace dxmt::this_thread {

bool isInModuleDetachment() {
  using PFN_RtlDllShutdownInProgress = BOOLEAN(WINAPI *)();

  static auto RtlDllShutdownInProgress =
      reinterpret_cast<PFN_RtlDllShutdownInProgress>(::GetProcAddress(
          ::GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress"));

  return RtlDllShutdownInProgress();
}

} // namespace dxmt::this_thread

#else

namespace dxmt::this_thread {

static std::atomic<uint32_t> g_threadCtr = {0u};
static thread_local uint32_t g_threadId = 0u;

// This implementation returns thread ids unique to the current instance.
// ie. if you use this across multiple .so's then you might get conflicting ids.
//
// This isn't an issue for us, as it is only used by the spinlock
// implementation, but may be for you if you use this elsewhere.
uint32_t get_id() {
  if (unlikely(!g_threadId))
    g_threadId = ++g_threadCtr;

  return g_threadId;
}

} // namespace dxmt::this_thread

#endif
