/* gw2 (#343, diagnostic build only): exception_log.c, for winemetal_unix.c's call tables and
   execute_on_main. */
#ifndef __WINEMETAL_UNIX_EXCEPTION_LOG_H
#define __WINEMETAL_UNIX_EXCEPTION_LOG_H

#include <dispatch/dispatch.h>

typedef int NTSTATUS;

/* runs handler(params) as bridge call `number`; logs an Objective-C exception that leaves it and
   throws it on */
NTSTATUS wmt_bridge_guarded(unsigned number, const char *name, const void *handler, void *params);

/* the bridge call running on this thread, NULL outside one */
const char *wmt_bridge_call(unsigned *number);

/* runs block on the main thread for bridge call `name` (execute_on_main), guarded as above; with
   no call, runs it unguarded */
void wmt_main_block_guarded(unsigned number, const char *name, dispatch_block_t block);

#endif
