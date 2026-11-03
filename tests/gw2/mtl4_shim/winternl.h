#pragma once
// gw2 (#32): the few Win32 types src/winemetal/wineunixlib.h needs, for compiling the bridge's PE side
// (winemetal_thunks.c) natively in tests/gw2/mtl4_queue_test.mm. Its WINE_UNIX_CALL then goes to the
// test's own __wine_unix_call_dispatcher instead of Wine's.
#include <stdint.h>
typedef uint64_t UINT64;
typedef int NTSTATUS;
#define WINAPI
