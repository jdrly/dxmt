/* gw2 (#291): the handlers of winemetal_gw2.c, for winemetal_unix.c's two call tables (their numbers:
   winemetal_calls.h). */
#ifndef __WINEMETAL_UNIX_GW2_H
#define __WINEMETAL_UNIX_GW2_H

typedef int NTSTATUS;

NTSTATUS _WMTSetThreadQoSClassSelf(void *obj);     /* gw2 (#9) */
NTSTATUS _WMTSetThreadBasePrioritySelf(void *obj); /* gw2 (#151) */

#ifdef __OBJC__
#include <stdbool.h>
/* gw2 (#341): sparseBufferTier on the buffer classes of `devices` (an NSArray of MTLDevice), for
   _MTLCopyAllDevices; the add on one class, for tests/gw2/sparse_buffer_tier_test.m */
void wmt_add_sparse_buffer_tiers(id devices);
bool wmt_add_sparse_buffer_tier(Class cls);
#endif

#endif
