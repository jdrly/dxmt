/* gw2 (#291): the handlers of winemetal_gw2.c, for winemetal_unix.c's two call tables (their numbers:
   winemetal_calls.h). */
#ifndef __WINEMETAL_UNIX_GW2_H
#define __WINEMETAL_UNIX_GW2_H

typedef int NTSTATUS;

NTSTATUS _WMTSetThreadQoSClassSelf(void *obj);     /* gw2 (#9) */
NTSTATUS _WMTSetThreadBasePrioritySelf(void *obj); /* gw2 (#151) */

#endif
