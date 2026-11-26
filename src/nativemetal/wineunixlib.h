#ifndef __WINE_WINE_UNIXLIB_H
#define __WINE_WINE_UNIXLIB_H

typedef int NTSTATUS;
#define NTSTATUS_SUCCESS 0

typedef NTSTATUS (*THUNKCALLBACK)(void *obj);

extern const THUNKCALLBACK __wine_unix_call_funcs[];

/* gw2 (B3, #76): a bridge call goes through the real table by its WMTCall number
   (winemetal_call_numbers.h, included before this header), and a reserved number returns an error
   instead of jumping to NULL (wmt_native_dispatch, src/winemetal/unix/winemetal_unix.c) */
NTSTATUS wmt_native_dispatch(enum WMTCall call, void *params);
#define WINE_UNIX_CALL(code, args) wmt_native_dispatch((code), (args))

#endif /* __WINE_WINE_UNIXLIB_H */
