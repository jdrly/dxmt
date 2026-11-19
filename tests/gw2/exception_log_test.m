// gw2-test: pure
// gw2-build: U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
// gw2-build: [[ " ${U[*]} " == *" exception_log.c "* ]]
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   for f in $U; do [[ -s $T/u_${f:r}_$arch.o ]] || clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Wno-unused-function -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $T/u_${f:r}_$arch.o; done
// gw2-build:   [[ -s $T/thunks_$arch.o ]] || clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal src/winemetal/winemetal_thunks.c -o $T/thunks_$arch.o
// gw2-build:   [[ -s $T/airconv_absent_$arch.o ]] || clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c -o $T/airconv_absent_$arch.o
// gw2-build:   clang -std=gnu17 -fno-objc-arc -fblocks -Wall -Wextra -Werror -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal -Isrc/winemetal/unix tests/gw2/exception_log_test.m $T/u_*_$arch.o $T/thunks_$arch.o $T/airconv_absent_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -lobjc -o ${OUT}_$arch
// gw2-build: done
// gw2-run: ${OUT}_x86_64
// gw2-run: ${OUT}_arm64
// gw2-run: rc=0; ${OUT}_arm64 --main-thread >$T/exception_log_main.out 2>&1 || rc=$?
// gw2-run: cat $T/exception_log_main.out
// gw2-run: echo "--main-thread exit status: $rc (expected: a signal, never 142, SIGALRM)"
// gw2-run: (( rc > 128 && rc != 142 ))
// gw2-run: grep -q 'out of bridge call MetalLayer_setProps (70) on the main thread$' $T/exception_log_main.out
// gw2-run: grep -q 'frames from the throw in MetalLayer_setProps:' $T/exception_log_main.out
// gw2-run: grep -q 'WMTMainThreadTest: forced on the main thread' $T/exception_log_main.out
// gw2-run: echo "RESULT --main-thread: exit $rc, $(grep -m1 -o 'out of bridge call MetalLayer_setProps.*' $T/exception_log_main.out)"
//
// gw2 (#343, diagnostic build only): forces Objective-C exceptions through the bridge and reads what
// src/winemetal/unix/exception_log.c logged. Needs no Metal device and no Wine.
//
// - NSArray_object (call 2), through the PE thunk and the production table (wmt_native_dispatch),
//   asks an empty NSMutableArray for index 5, and CoreFoundation throws NSRangeException inside the
//   handler. The same exception reaches the caller, and the log has the call, the frames from the
//   throw through libobjc, CoreFoundation and this program, the name and reason, and both stacks.
// - 12 exceptions thrown outside a bridge call: 8 logged, as raw PCs.
// - --main-thread: a bridge call on another thread whose execute_on_main block raises on the main
//   thread (a run loop there), as MetalLayer_setProps's would. Its log names that call. libdispatch
//   ends the process on an exception out of a block, as it would in the game, so the run lines
//   expect it to die of a signal, and not of the alarm that stops a hang.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#import <Foundation/Foundation.h>
#include "winemetal.h"
#include "exception_log.h"

void execute_on_main(dispatch_block_t block); // winemetal_unix.c

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

// the lines from the first that contains start through the first after it that contains end
static NSArray<NSString *> *
lines_between(NSArray<NSString *> *lines, NSString *start, NSString *end) {
  NSMutableArray<NSString *> *out = [NSMutableArray array];
  for (NSString *line in lines) {
    if (![out count] && ![line containsString:start])
      continue;
    [out addObject:line];
    if ([out count] > 1 && [line containsString:end])
      break;
  }
  return out;
}

static NSString *
first_with(NSArray<NSString *> *lines, NSString *part) {
  for (NSString *line in lines)
    if ([line containsString:part])
      return line;
  return nil;
}

static NSUInteger
count_with(NSArray<NSString *> *lines, NSString *part) {
  NSUInteger n = 0;
  for (NSString *line in lines)
    n += [line containsString:part];
  return n;
}

static NTSTATUS
raise_on_main(void *params) {
  (void)params;
  execute_on_main(^{
    [NSException raise:@"WMTMainThreadTest" format:@"forced on the %s thread", [NSThread isMainThread] ? "main" : "wrong"];
  });
  return 0;
}

static void *
set_props_thread(void *arg) {
  (void)arg;
  wmt_bridge_guarded(70, "MetalLayer_setProps", (const void *)raise_on_main, NULL);
  return NULL;
}

int
main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--main-thread")) {
    alarm(30); // ends the run if the process outlives the exception
    pthread_t thread;
    pthread_create(&thread, NULL, set_props_thread, NULL);
    CFRunLoopRun();
    return 1;
  }
  @autoreleasepool {
    const char *self = strrchr(argv[0], '/');
    NSString *program = [NSString stringWithUTF8String:self ? self + 1 : argv[0]];

    int saved = dup(2);
    FILE *capture = tmpfile();
    CHECK(saved >= 0 && capture);
    if (!capture)
      return 1;
    dup2(fileno(capture), 2);

    NSMutableArray *empty = [[NSMutableArray alloc] init];
    NSString *caught = nil;
    @try {
      NSArray_object((obj_handle_t)empty, 5);
    } @catch (NSException *e) {
      caught = [[e name] copy];
    }
    for (int i = 0; i < 12; i++) {
      @try {
        [NSException raise:@"WMTOutsideTest" format:@"forced, %d", i];
      } @catch (NSException *e) {
        (void)e;
      }
    }

    dup2(saved, 2);
    close(saved);
    long size = ftell(capture);
    char *text = calloc(1, size > 0 ? size + 1 : 1);
    rewind(capture);
    size_t got = size > 0 ? fread(text, 1, size, capture) : 0;
    fclose(capture);
    NSString *log = [[NSString alloc] initWithBytes:text length:got encoding:NSUTF8StringEncoding];
    free(text);
    printf("%s", [log UTF8String]);
    NSArray<NSString *> *lines = [log componentsSeparatedByString:@"\n"];

    CHECK([caught isEqualToString:NSRangeException]);

    NSArray<NSString *> *out =
        lines_between(lines, @"out of bridge call NSArray_object (2)", @"end of bridge call NSArray_object");
    CHECK([out count] > 5);
    CHECK([[out lastObject] hasSuffix:@"end of bridge call NSArray_object"]);
    CHECK(first_with(out, @"frames from the throw in NSArray_object:") != nil);
    CHECK(first_with(out, @" libobjc.A.dylib+") != nil);
    CHECK(first_with(out, @" CoreFoundation+") != nil);
    CHECK(first_with(out, [NSString stringWithFormat:@" %@+", program]) != nil);
    CHECK(first_with(out, @"NSRangeException: ") != nil);
    CHECK(first_with(out, @"the exception's stack (") != nil);
    CHECK(first_with(out, @"the thread's stack at the catch (") != nil);
    NSUInteger logged = count_with(lines, @"thrown outside a bridge call");
    CHECK(logged == 8);

    printf("RESULT %s\n", [first_with(out, @"out of bridge call") UTF8String] ?: "(no catch block)");
    printf("RESULT %s\n", [first_with(out, @" CoreFoundation+") UTF8String] ?: "(no CoreFoundation frame)");
    printf("RESULT %s\n", [first_with(out, @"NSRangeException: ") UTF8String] ?: "(no reason)");
    printf("RESULT exceptions outside a bridge call logged: %lu of 12\n", (unsigned long)logged);

    [empty release];
    [caught release];
    [log release];
  }
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("exception log: OK\n");
  return 0;
}
