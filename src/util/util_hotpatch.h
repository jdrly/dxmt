/*
 * Derived from DXVK (originally under zlib License)
 *
 * See
 * <https://github.com/doitsujin/dxvk/blob/8c4fd2723c031b425fb857454e9e5fa4ca25f4ac/src/util/util_hotpatch.h>
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#if defined(DECLSPEC_CHPE_PATCHABLE)
#define DXMT_HOTPATCHABLE DECLSPEC_CHPE_PATCHABLE
#elif defined(__arm64ec__)
#define DXMT_HOTPATCHABLE __attribute__((hybrid_patchable))
#else
#define DXMT_HOTPATCHABLE
#endif

// gw2 (#248): a patchable entry whose body another patchable entry needs. On ARM64EC a call to a
// DXMT_HOTPATCHABLE function goes through its patchable x64 entry even from inside this module, so
// an entry that called another ran a hook on both of them twice: with Present calling Present1, a
// hook on both (the Steam overlay's) ran twice a frame. So
//
//   DXMT_HOTPATCHABLE_SHARED(Present1, PresentFrame, (UINT a, UINT b, ...), (a, b, ...)) { body }
//
// declares the entry Present1 and PresentFrame, which has its body, and the other entries call
// PresentFrame. On ARM64EC Present1 calls PresentFrame. Elsewhere the attribute is empty: Present1
// keeps the body and PresentFrame is an inlined call to it, so those builds compile as before.
// .github/gw2/check-mach9.zsh fails when an entry's ARM64EC code reaches another entry.
#if defined(__arm64ec__)
#define DXMT_HOTPATCHABLE_SHARED(entry, body, params, args)                                         \
  DXMT_HOTPATCHABLE HRESULT STDMETHODCALLTYPE entry params final { return body args; }            \
  HRESULT body params
#else
#define DXMT_HOTPATCHABLE_SHARED(entry, body, params, args)                                         \
  __attribute__((always_inline)) HRESULT body params { return entry args; }                       \
  DXMT_HOTPATCHABLE HRESULT STDMETHODCALLTYPE entry params final
#endif
