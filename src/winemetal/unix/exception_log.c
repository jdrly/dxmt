/*
 * gw2 (#343, diagnostic build only, never merged into gw2-on-mac): logs the Objective-C exceptions
 * that pass through the bridge, for #341's sparseBufferTier crash, where wine.log had an empty
 * "First throw call stack". Every line goes to stderr (the game's wine.log) with one write(2),
 * starts with "winemetal: exception: [<thread id>]" and is cut at 512 bytes. No stdio, no lock.
 *
 * - At the throw, an exception preprocessor chained to CoreFoundation's walks the frame pointers
 *   from the throw. It reads each frame through vm_read_overwrite, so a broken chain ends the walk
 *   and can't fault, and it doesn't keep to the pthread stack bounds that Apple's walker keeps to.
 *   It sends no message, takes no lock and calls no dladdr.
 *   - Inside a bridge call it only keeps the PCs in a buffer of the thread's, for the catch below.
 *   - Outside one (an exception thrown and caught on Metal's own threads), it logs the PCs against
 *     the images captured at load and after each MTLCopyAllDevices, at most 8 per process.
 * - At the bridge, wmt_bridge_guarded runs each call's handler in @try. When an exception leaves the
 *   handler, it logs the call, the PCs from the throw named with dladdr, and then, in a @try of its
 *   own, the exception's name and reason, its stack, the thread's stack, and the class of the
 *   call's receiver. Then @throw passes the exception on unchanged.
 * - execute_on_main runs its block on the main thread under the same guard, named for the bridge
 *   call that asked for it (MetalLayer_setProps, MetalLayer_setColorSpace).
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
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
#include <objc/runtime.h>
#include "exception_log.h"

#define FRAMES_MAX 64
#define THROWN_OUTSIDE_MAX 8
#define IMAGES_MAX 4096
#define MTL_COPY_ALL_DEVICES 4 /* winemetal_calls.h */

static __thread const char *current_call;
static __thread unsigned current_number;
static __thread int in_log; /* an exception raised while logging one isn't recorded */
/* the PCs of the last exception thrown on this thread inside a bridge call */
static __thread struct {
  id exception;
  unsigned count;
  uintptr_t pcs[FRAMES_MAX];
} thrown;

static objc_exception_preprocessor previous_preprocessor;
static _Atomic unsigned thrown_outside;

/* the loaded images: start and end of __TEXT, and dyld's path */
static struct {
  uintptr_t start, end;
  const char *path;
} images[IMAGES_MAX];
static _Atomic unsigned image_count;
static atomic_flag capturing = ATOMIC_FLAG_INIT;

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

static void
capture_images(void) {
  if (atomic_flag_test_and_set(&capturing))
    return;
  uint32_t count = _dyld_image_count();
  unsigned n = 0;
  for (uint32_t i = 0; i < count && n < IMAGES_MAX; i++) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
    const char *path = _dyld_get_image_name(i);
    if (!mh || !path || mh->magic != MH_MAGIC_64)
      continue;
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t c = 0; c < mh->ncmds; c++) {
      if (lc->cmd == LC_SEGMENT_64 && !strncmp(((const struct segment_command_64 *)lc)->segname, "__TEXT", 16)) {
        images[n].start = (uintptr_t)mh;
        images[n].end = (uintptr_t)mh + ((const struct segment_command_64 *)lc)->vmsize;
        images[n].path = path;
        n++;
        break;
      }
      lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
    }
  }
  atomic_store_explicit(&image_count, n, memory_order_release);
  atomic_flag_clear(&capturing);
}

static const char *
base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* outside a bridge call: the image table only */
static void
out_pc_raw(unsigned index, uintptr_t pc) {
  unsigned count = atomic_load_explicit(&image_count, memory_order_acquire);
  for (unsigned i = 0; i < count; i++) {
    if (pc >= images[i].start && pc < images[i].end) {
      out("     %2u 0x%016lx %s+0x%lx (image at 0x%lx)", index, (unsigned long)pc, base_name(images[i].path),
          (unsigned long)(pc - images[i].start), (unsigned long)images[i].start);
      return;
    }
  }
  out("     %2u 0x%016lx ?", index, (unsigned long)pc);
}

/* in the catch: dladdr */
static void
out_pc(unsigned index, uintptr_t pc) {
  Dl_info info;
  if (!dladdr((const void *)pc, &info) || !info.dli_fname)
    out("     %2u 0x%016lx ?", index, (unsigned long)pc);
  else if (info.dli_sname)
    out("     %2u 0x%016lx %s+0x%lx %s+0x%lx", index, (unsigned long)pc, base_name(info.dli_fname),
        (unsigned long)(pc - (uintptr_t)info.dli_fbase), info.dli_sname, (unsigned long)(pc - (uintptr_t)info.dli_saddr));
  else
    out("     %2u 0x%016lx %s+0x%lx", index, (unsigned long)pc, base_name(info.dli_fname),
        (unsigned long)(pc - (uintptr_t)info.dli_fbase));
}

static id
preprocess(id e) {
  if (previous_preprocessor)
    e = previous_preprocessor(e);
  if (in_log)
    return e;
  if (current_call) {
    thrown.exception = e;
    thrown.count = walk_frames((uintptr_t)__builtin_frame_address(0), thrown.pcs);
    return e;
  }
  if (atomic_fetch_add(&thrown_outside, 1) >= THROWN_OUTSIDE_MAX)
    return e;
  uintptr_t pcs[FRAMES_MAX];
  unsigned n = walk_frames((uintptr_t)__builtin_frame_address(0), pcs);
  out(" thrown outside a bridge call: a %s, %u frames from the throw:", object_getClassName(e), n);
  for (unsigned i = 0; i < n; i++)
    out_pc_raw(i, pcs[i]);
  return e;
}

__attribute__((constructor)) static void
install_preprocessor(void) {
  capture_images();
  previous_preprocessor = objc_setExceptionPreprocessor(preprocess);
}

static void
out_symbols(const char *what, NSArray<NSString *> *symbols) {
  out("   %s (%lu frames):", what, (unsigned long)[symbols count]);
  for (NSString *s in symbols)
    out("     %s", [s UTF8String]);
}

/* calls named Class_method pass their receiver, an Objective-C object, as the first word of params
   (winemetal_thunks.h); these few pass an output there instead */
static bool
receiver_first(const char *name) {
  static const char *const outputs[] = {
      "MTLCaptureManager_sharedCaptureManager",
      "NSAutoreleasePool_alloc_init",
      "NSString_string",
      "NSString_alloc_init",
  };
  if (strncmp(name, "MTL", 3) && strncmp(name, "NS", 2) && strncmp(name, "MetalLayer_", 11) &&
      strncmp(name, "MetalDrawable_", 14))
    return false;
  if (!strchr(name, '_') || strchr(name, ' '))
    return false;
  for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++)
    if (!strcmp(name, outputs[i]))
      return false;
  return true;
}

static void
log_escape(unsigned number, const char *name, const char *where, id e, const void *params) {
  if (in_log)
    return;
  in_log = 1;
  out(" out of bridge call %s (%u)%s: a %s", name, number, where, object_getClassName(e));
  if (thrown.exception == e && thrown.count) {
    out("   %u frames from the throw:", thrown.count);
    for (unsigned i = 0; i < thrown.count; i++)
      out_pc(i, thrown.pcs[i]);
  } else {
    out("   no frames kept at the throw for this exception");
  }
  thrown.exception = nil;
  thrown.count = 0;
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  @try {
    if ([e isKindOfClass:[NSException class]]) {
      NSException *ex = e;
      out("   %s: %s", [[ex name] UTF8String] ?: "(no name)", [[ex reason] UTF8String] ?: "(no reason)");
      out_symbols("the exception's stack", [ex callStackSymbols]);
    }
    out_symbols("the thread's stack at the catch", [NSThread callStackSymbols]);
    if (params && receiver_first(name)) {
      uint64_t receiver = *(const uint64_t *)params;
      out("   the call's receiver: 0x%llx, a %s", (unsigned long long)receiver,
          receiver ? object_getClassName((id)receiver) : "nil");
    }
  } @catch (id inner) {
    out("   logging stopped: a %s raised while logging", object_getClassName(inner));
  }
  [pool drain];
  out(" end of bridge call %s", name);
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
    if (number == MTL_COPY_ALL_DEVICES)
      capture_images(); /* the GPU's driver bundle is loaded now */
    return status;
  } @catch (id e) {
    current_call = outer_call;
    current_number = outer_number;
    log_escape(number, name, "", e, params);
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
    log_escape(number, name, " on the main thread", e, NULL);
    @throw;
  }
}
