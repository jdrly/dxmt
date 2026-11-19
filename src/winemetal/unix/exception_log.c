/*
 * gw2 (#343, diagnostic build only, never merged into gw2-on-mac): logs the Objective-C exceptions
 * that pass through the bridge, for #341's sparseBufferTier crash, where wine.log had an empty
 * "First throw call stack".
 *
 * Two logs, both to stderr (the game's wine.log), each line starting with "winemetal: exception:":
 *
 * - At the throw: an exception preprocessor, chained to the one CoreFoundation installed, names the
 *   bridge call running on the thread (or says there is none) and walks the frame pointers from the
 *   throw. It reads every frame through vm_read_overwrite, so a broken chain ends the walk and can't
 *   fault, and it doesn't depend on the pthread stack bounds that Apple's walker keeps to. Thrown
 *   inside a bridge call: the first 32 per process. Outside one (Metal's own threads, blocks on the
 *   main queue): the first 8.
 * - At the bridge: wmt_bridge_guarded runs each call's handler in @try. An exception that leaves the
 *   handler is logged (call, name, reason, the exception's and the thread's stacks, the params
 *   words with the class of each word that is an Objective-C object) and thrown on with @throw, so
 *   what happens next is what happened without this file.
 */
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#import <Foundation/Foundation.h>
#include <objc/objc-exception.h>
#include <objc/runtime.h>

typedef int NTSTATUS;

#define TAG "winemetal: exception:"
#define THROWN_IN_CALL_MAX 32
#define THROWN_OUTSIDE_MAX 8
#define FRAMES_MAX 64
#define PARAMS_WORDS 8

static __thread const char *current_call;
static __thread unsigned current_number;
static __thread int in_log; /* an exception thrown while logging one isn't logged */
static os_unfair_lock log_lock = OS_UNFAIR_LOCK_INIT;
static objc_exception_preprocessor previous_preprocessor;
static _Atomic unsigned thrown_in_call, thrown_outside;

/* one word at addr, or false where it isn't readable */
static bool
read_word(uintptr_t addr, uintptr_t *out) {
  vm_size_t got = 0;
  return vm_read_overwrite(mach_task_self(), (vm_address_t)addr, sizeof(*out), (vm_address_t)out, &got) ==
             KERN_SUCCESS &&
         got == sizeof(*out);
}

static void
print_address(unsigned index, uintptr_t pc) {
  Dl_info info;
  if (dladdr((const void *)pc, &info) && info.dli_fname) {
    const char *image = strrchr(info.dli_fname, '/');
    image = image ? image + 1 : info.dli_fname;
    if (info.dli_sname)
      fprintf(stderr, TAG "     %2u 0x%016lx %s+0x%lx %s+0x%lx\n", index, (unsigned long)pc, image,
              (unsigned long)(pc - (uintptr_t)info.dli_fbase), info.dli_sname,
              (unsigned long)(pc - (uintptr_t)info.dli_saddr));
    else
      fprintf(stderr, TAG "     %2u 0x%016lx %s+0x%lx\n", index, (unsigned long)pc, image,
              (unsigned long)(pc - (uintptr_t)info.dli_fbase));
  } else {
    fprintf(stderr, TAG "     %2u 0x%016lx ?\n", index, (unsigned long)pc);
  }
}

/* the frame pointer chain from the caller's frame: [fp] is the caller's fp, [fp + 8] the return
   address. Apple's arm64e code signs return addresses; the pointer is the low 47 bits. */
static void
walk_frames(uintptr_t fp) {
  unsigned n = 0;
  while (fp && !(fp & 7) && n < FRAMES_MAX) {
    uintptr_t next, lr;
    if (!read_word(fp, &next) || !read_word(fp + sizeof(uintptr_t), &lr) || !lr)
      break;
#ifdef __aarch64__
    lr &= 0x00007fffffffffffULL;
#endif
    print_address(n++, lr);
    if (next <= fp)
      break;
    fp = next;
  }
  fprintf(stderr, TAG "   %u frames walked\n", n);
}

static void
print_exception(id e) {
  if ([e isKindOfClass:[NSException class]]) {
    NSException *ex = e;
    fprintf(stderr, TAG "   %s: %s\n", [[ex name] UTF8String] ?: "(no name)",
            [[ex reason] UTF8String] ?: "(no reason)");
  } else {
    fprintf(stderr, TAG "   a thrown %s, not an NSException\n", object_getClassName(e));
  }
}

static uint64_t
thread_id(void) {
  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);
  return tid;
}

static id
preprocess(id e) {
  if (previous_preprocessor)
    e = previous_preprocessor(e);
  if (in_log)
    return e;
  const char *call = current_call;
  if (call ? atomic_fetch_add(&thrown_in_call, 1) >= THROWN_IN_CALL_MAX
           : atomic_fetch_add(&thrown_outside, 1) >= THROWN_OUTSIDE_MAX)
    return e;
  in_log = 1;
  os_unfair_lock_lock(&log_lock);
  if (call)
    fprintf(stderr, TAG " thrown on thread %llu in bridge call %s (%u)\n", (unsigned long long)thread_id(), call,
            current_number);
  else
    fprintf(stderr, TAG " thrown on thread %llu outside a bridge call\n", (unsigned long long)thread_id());
  print_exception(e);
  fprintf(stderr, TAG "   frames from the throw:\n");
  walk_frames((uintptr_t)__builtin_frame_address(0));
  fflush(stderr);
  os_unfair_lock_unlock(&log_lock);
  in_log = 0;
  return e;
}

__attribute__((constructor)) static void
install_preprocessor(void) {
  previous_preprocessor = objc_setExceptionPreprocessor(preprocess);
}

static void
print_symbols(const char *what, NSArray<NSString *> *symbols) {
  fprintf(stderr, TAG "   %s (%lu frames):\n", what, (unsigned long)[symbols count]);
  for (NSString *s in symbols)
    fprintf(stderr, TAG "     %s\n", [s UTF8String]);
}

/* the params words, and the class of each that is an Objective-C object: a malloc block whose isa
   is a class the runtime lists. Nothing here dereferences a word that isn't one. */
static void
print_params(const void *params) {
  int count = objc_getClassList(NULL, 0);
  Class *classes = count > 0 ? malloc(sizeof(Class) * count) : NULL;
  if (classes)
    count = objc_getClassList(classes, count);
  for (unsigned i = 0; i < PARAMS_WORDS; i++) {
    uintptr_t word;
    if (!read_word((uintptr_t)params + i * sizeof(word), &word)) {
      fprintf(stderr, TAG "   params word %u: unreadable\n", i);
      break;
    }
    const char *kind = "";
    size_t size = word ? malloc_size((const void *)word) : 0;
    if (size >= sizeof(uintptr_t)) {
      Class cls = object_getClass((id)word);
      kind = "a malloc block, not an object";
      for (int c = 0; classes && c < count; c++) {
        if (classes[c] == cls) {
          kind = class_getName(cls);
          break;
        }
      }
    }
    fprintf(stderr, TAG "   params word %u: 0x%016lx %s\n", i, (unsigned long)word, kind);
  }
  free(classes);
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
    if (!in_log) {
      in_log = 1;
      os_unfair_lock_lock(&log_lock);
      @autoreleasepool {
        fprintf(stderr, TAG " out of bridge call %s (%u) on thread %llu\n", name, number,
                (unsigned long long)thread_id());
        print_exception(e);
        if ([e isKindOfClass:[NSException class]])
          print_symbols("the exception's stack", [(NSException *)e callStackSymbols]);
        print_symbols("the thread's stack at the catch", [NSThread callStackSymbols]);
        fflush(stderr);
        print_params(params);
        fprintf(stderr, TAG " end of bridge call %s\n", name);
        fflush(stderr);
      }
      os_unfair_lock_unlock(&log_lock);
      in_log = 0;
    }
    @throw;
  }
}
