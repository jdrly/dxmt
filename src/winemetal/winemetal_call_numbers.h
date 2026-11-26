#ifndef __WINEMETAL_CALL_NUMBERS_H
#define __WINEMETAL_CALL_NUMBERS_H

/*
 * gw2 (B2, #67; B3, #76): the bridge's call numbers, generated from the manifest (winemetal_calls.h).
 * WMTCall_<name> is a call's number. A hole is WMTReserved_<number>, outside WMTCall_ so that no
 * thunk can name it (B3). The PE thunks name their calls by it (UNIX_CALL(NSObject_retain, ...) in
 * winemetal_thunks.c and airconv_thunks.c), and the unix side's two tables come from the same lines.
 * It has its own header (B3) so that airconv_thunks.h gets the numbers without winemetal.h;
 * winemetal_thunks.h includes it too.
 */
enum WMTCall {
#define WMT_CALL(number, name, handler) WMTCall_##name = number,
#define WMT_CALL_WOW64(number, name, handler, wow64_handler) WMTCall_##name = number,
#define WMT_RESERVED(number, reserved_by) WMTReserved_##number = number,
#include "winemetal_calls.h"
  WMTCall_Count /* the tables' length: one more than the last line's number */
};

/* Each line's position in the manifest, counted from 0: equal to its number on every line only if
   the numbers have no duplicate, no gap and no line out of order. */
enum WMTCallPosition {
#define WMT_CALL(number, name, handler) WMTCallPosition_##name,
#define WMT_CALL_WOW64(number, name, handler, wow64_handler) WMTCallPosition_##name,
#define WMT_RESERVED(number, reserved_by) WMTCallPosition_reserved_##number,
#include "winemetal_calls.h"
  WMTCallPosition_Count
};

#ifdef __cplusplus
#define WMT_CALLS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define WMT_CALLS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#define WMT_CALL(number, name, handler)                                                                                \
  WMT_CALLS_STATIC_ASSERT(                                                                                             \
      WMTCallPosition_##name == number, "winemetal_calls.h: " #name " is not at its call number " #number              \
                                        " (a duplicate, a gap or a line out of order)"                                 \
  );
#define WMT_CALL_WOW64(number, name, handler, wow64_handler) WMT_CALL(number, name, handler)
#define WMT_RESERVED(number, reserved_by)                                                                              \
  WMT_CALLS_STATIC_ASSERT(                                                                                             \
      WMTCallPosition_reserved_##number == number, "winemetal_calls.h: reserved " #number                              \
                                                   " is not at its call number (a duplicate, a gap or a line out of "  \
                                                   "order)"                                                            \
  );
#include "winemetal_calls.h"
WMT_CALLS_STATIC_ASSERT(WMTCall_Count == (int)WMTCallPosition_Count, "winemetal_calls.h: numbers are not 0..Count-1");
/* Fixed anchors from before the manifest, so the checks don't only compare the manifest with itself */
WMT_CALLS_STATIC_ASSERT(WMTCall_NSObject_retain == 0, "call number 0 moved");
WMT_CALLS_STATIC_ASSERT(WMTCall_MetalView_visibility == 147, "call number 147 moved");
WMT_CALLS_STATIC_ASSERT(WMTCall_MTLDevice_metal4Support == 155, "call number 155 moved");
#undef WMT_CALLS_STATIC_ASSERT

#endif /* __WINEMETAL_CALL_NUMBERS_H */
