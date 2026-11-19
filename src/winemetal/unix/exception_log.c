/*
 * gw2 (#343, diagnostic build only, never merged into gw2-on-mac): logs the Objective-C exceptions
 * that pass through the bridge, for #341's sparseBufferTier crash, where wine.log had an empty
 * "First throw call stack". Every line goes to stderr (the game's wine.log) with one write(2),
 * starts with "winemetal: exception: [<thread id>]" and is cut at 512 bytes. No stdio, no lock.
 *
 * - At the throw, an exception preprocessor chained to CoreFoundation's walks the frame pointers
 *   from the throw. It reads each frame through vm_read_overwrite, so a broken chain ends the walk
 *   and can't fault, and it doesn't keep to the pthread stack bounds that Apple's walker keeps to.
 *   Inside a bridge call it keeps the PCs and the call's name in a buffer of the thread's for the
 *   catch below. Outside one, it logs the raw PCs, at most 8 times per process.
 * - At the bridge, wmt_bridge_guarded runs each call's handler in @try. When an exception leaves the
 *   handler, it logs the call, the PCs from the throw named with dladdr, the exception's name and
 *   reason, its stack and the thread's stack, all inside a @try of their own. Then @throw passes the
 *   exception on unchanged.
 * - execute_on_main runs its block on the main thread under the same guard, named for the bridge
 *   call that asked for it (MetalLayer_setProps, MetalLayer_setColorSpace).
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#import <Foundation/Foundation.h>
#include <objc/objc-exception.h>
#include "exception_log.h"

#define FRAMES_MAX 64
#define THROWN_OUTSIDE_MAX 8

static __thread const char *current_call;
static __thread unsigned current_number;
static __thread int in_log; /* an exception raised while logging one isn't recorded */
/* the last exception thrown on this thread inside a bridge call */
static __thread struct {
  id exception; /* compared, never sent a message */
  const char *call;
  unsigned count;
  uintptr_t pcs[FRAMES_MAX];
} thrown;

static objc_exception_preprocessor previous_preprocessor;
static _Atomic unsigned thrown_outside;

static void __attribute__((format(printf, 1, 2)))
out(const char *format, ...) {
  char line[512];
  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);
  int n = snprintf(line, sizeof(line) - 1, "winemetal: exception: [%llu] ", (unsigned long long)tid);
  va_list args;
  va_start(args, format);
  int m = vsnprintf(line + n, sizeof(line) - 1 - n, format, args);
  va_end(args);
  n = m < 0 ? n : (n + m > (int)sizeof(line) - 2 ? (int)sizeof(line) - 2 : n + m);
  line[n++] = '\n';
  (void)!write(2, line, n);
}

/* one word at addr, or false where it isn't readable */
static bool
read_word(uintptr_t addr, uintptr_t *word) {
  vm_size_t got = 0;
  return vm_read_overwrite(mach_task_self(), (vm_address_t)addr, sizeof(*word), (vm_address_t)word, &got) ==
             KERN_SUCCESS &&
         got == sizeof(*word);
}

/* the frame pointer chain from fp: [fp] is the caller's fp, [fp + 8] the return address. Apple's
   arm64e code signs return addresses; the address is the low 47 bits. */
static unsigned
walk_frames(uintptr_t fp, uintptr_t *pcs) {
  unsigned n = 0;
  while (fp && !(fp & 7) && n < FRAMES_MAX) {
    uintptr_t next, lr;
    if (!read_word(fp, &next) || !read_word(fp + sizeof(uintptr_t), &lr) || !lr)
      break;
#ifdef __aarch64__
    lr &= 0x00007fffffffffffULL;
#endif
    pcs[n++] = lr;
    if (next <= fp)
      break;
    fp = next;
  }
  return n;
}

static id
preprocess(id e) {
  if (previous_preprocessor)
    e = previous_preprocessor(e);
  if (in_log)
    return e;
  if (current_call) {
    thrown.exception = e;
    thrown.call = current_call;
    thrown.count = walk_frames((uintptr_t)__builtin_frame_address(0), thrown.pcs);
    return e;
  }
  if (atomic_fetch_add(&thrown_outside, 1) >= THROWN_OUTSIDE_MAX)
    return e;
  uintptr_t pcs[FRAMES_MAX];
  unsigned n = walk_frames((uintptr_t)__builtin_frame_address(0), pcs);
  out(" thrown outside a bridge call, %u frames from the throw (raw):", n);
  for (unsigned i = 0; i < n; i++)
    out("     %2u 0x%016lx", i, (unsigned long)pcs[i]);
  return e;
}

__attribute__((constructor)) static void
install_preprocessor(void) {
  previous_preprocessor = objc_setExceptionPreprocessor(preprocess);
}

static void
out_pc(unsigned index, uintptr_t pc) {
  Dl_info info;
  if (!dladdr((const void *)pc, &info) || !info.dli_fname) {
    out("     %2u 0x%016lx ?", index, (unsigned long)pc);
    return;
  }
  const char *image = strrchr(info.dli_fname, '/');
  image = image ? image + 1 : info.dli_fname;
  if (info.dli_sname)
    out("     %2u 0x%016lx %s+0x%lx %s+0x%lx", index, (unsigned long)pc, image,
        (unsigned long)(pc - (uintptr_t)info.dli_fbase), info.dli_sname, (unsigned long)(pc - (uintptr_t)info.dli_saddr));
  else
    out("     %2u 0x%016lx %s+0x%lx", index, (unsigned long)pc, image, (unsigned long)(pc - (uintptr_t)info.dli_fbase));
}

static void
out_symbols(const char *what, NSArray<NSString *> *symbols) {
  out("   %s (%lu frames):", what, (unsigned long)[symbols count]);
  for (NSString *s in symbols)
    out("     %s", [s UTF8String]);
}

/* everything inside one @try: an exception raised while logging ends the log, never the caller's
   rethrow */
static void
log_escape(unsigned number, const char *name, const char *where, id e) {
  if (in_log)
    return;
  in_log = 1;
  @try {
    out(" out of bridge call %s (%u)%s", name, number, where);
    if (thrown.exception == e && thrown.count) {
      out("   %u frames from the throw in %s:", thrown.count, thrown.call);
      for (unsigned i = 0; i < thrown.count; i++)
        out_pc(i, thrown.pcs[i]);
    } else {
      out("   no frames kept at the throw for this exception");
    }
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    if ([e isKindOfClass:[NSException class]]) {
      NSException *ex = e;
      out("   %s: %s", [[ex name] UTF8String] ?: "(no name)", [[ex reason] UTF8String] ?: "(no reason)");
      out_symbols("the exception's stack", [ex callStackSymbols]);
    }
    out_symbols("the thread's stack at the catch", [NSThread callStackSymbols]);
    [pool drain];
    out(" end of bridge call %s", name);
  } @catch (id inner) {
    (void)inner;
    out(" end of bridge call %s: an exception raised while logging stopped the log", name);
  }
  thrown.exception = nil;
  thrown.count = 0;
  in_log = 0;
}

NTSTATUS
wmt_bridge_guarded(unsigned number, const char *name, const void *handler, void *params) {
  const char *outer_call = current_call;
  unsigned outer_number = current_number;
  current_call = name;
  current_number = number;
  @try {
    NTSTATUS status = ((NTSTATUS(*)(void *))handler)(params);
    current_call = outer_call;
    current_number = outer_number;
    return status;
  } @catch (id e) {
    current_call = outer_call;
    current_number = outer_number;
    log_escape(number, name, "", e);
    @throw;
  }
}

const char *
wmt_bridge_call(unsigned *number) {
  *number = current_number;
  return current_call;
}

void
wmt_main_block_guarded(unsigned number, const char *name, dispatch_block_t block) {
  if (!name) {
    block();
    return;
  }
  const char *outer_call = current_call;
  unsigned outer_number = current_number;
  current_call = name;
  current_number = number;
  @try {
    block();
    current_call = outer_call;
    current_number = outer_number;
  } @catch (id e) {
    current_call = outer_call;
    current_number = outer_number;
    log_escape(number, name, " on the main thread", e);
    @throw;
  }
}
