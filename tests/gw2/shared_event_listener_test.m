// gw2-test: metal
// gw2-build: awk '/^#ifndef DXMT_NATIVE/{inb=1; buf=""; next} inb && /^#else/{if (buf ~ /_SharedEventListener_destroy/) {printf "%s", buf; exit} inb=0} inb{buf=buf $0 "\n"}' src/winemetal/unix/winemetal_unix.c >$T/shared_event_listener.inc
// gw2-build: grep -q '^_SharedEventListener_destroy' $T/shared_event_listener.inc
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   san=(); [[ $arch == arm64 ]] && san=(-fsanitize=address)
// gw2-build:   clang -x objective-c -std=gnu17 -fblocks -fno-objc-arc -O1 -g -Wall -Wno-unused-function $san -arch $arch -mmacosx-version-min=26.0 -Isrc/winemetal -DLISTENER_SRC="\"$T/shared_event_listener.inc\"" tests/gw2/shared_event_listener_test.m -framework Metal -framework Foundation -framework CoreFoundation -o ${OUT}_$arch
// gw2-build: done
// gw2-run: ${OUT}_x86_64
// gw2-run: ${OUT}_arm64
// gw2-run: rc=0; LISTENER_TEST_NO_DEVICE=1 ${OUT}_x86_64 || rc=$?; (( rc == 77 )) || { echo "FAIL no device: exit $rc, not 77"; exit 1 }
//
// gw2 (#127, port of athei/dxmt 0cd8b76e9a): native test of the shared-event listener's lifetime
// (winemetal_unix.c: _SharedEventListener_create/start/destroy and _MTLSharedEvent_setWin32EventAtValue,
// bridge calls 108-110 and the setWin32EventAtValue call). Those handlers are compiled only into the
// Wine build (#ifndef DXMT_NATIVE), so the build lines cut that block out of the real source and
// include it here; NtSetEvent is this file's stand-in, which records where and how often it ran.
// No Wine, no game, no window. The arm64 binary runs under AddressSanitizer.
//
// The PE side runs start on a listener thread of its own (dxmt::CommandQueue, dxmt::EventListener)
// and destroy on the thread that tears the queue down, then joins the listener thread. This test
// does the same with pthreads, and fails if a join takes longer than the watchdog allows (a hang
// can't be undone) or if ASan sees a freed listener used:
//
// 1. destroy before start, 300 times: start must return at once (before the fix it ran a run loop
//    nothing stopped, and the join hung).
// 2. destroy racing start, 3,000 times with 0-200 us between them: every join returns (before the
//    fix, a CFRunLoopStop before CFRunLoopRun was lost and the join hung; destroy also freed the
//    listener start was using).
// 3. a notification reaches NtSetEvent on the listener thread, once per signalled value.
// 4. a notification signalled before start is delivered once start runs.
// 5. a notification still pending at destroy fires after it: it is dropped, NtSetEvent is not
//    called, and nothing reads freed memory (before the fix, ASan: heap-use-after-free).
// Parts 3-5 need a Metal device; parts 1-2 never do. Without a device the test still runs parts 1-2
// (a failure there exits 1), then exits 77, so the runner reports it as skipped, never as passed
// (#131: it used to exit 0 with parts 3-5 not run). LISTENER_TEST_NO_DEVICE=1 takes that path on a
// Mac with a device; the run lines check its 77.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreFoundation/CFRunLoop.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define WINEMETAL_API
#include "winemetal_thunks.h"

typedef int NTSTATUS;
#define STATUS_SUCCESS 0

// the stand-in for Wine's NtSetEvent: counts calls per handle and notes the thread
static _Atomic(int) set_count[8];
static _Atomic(int) set_off_listener_thread;
static _Atomic(pthread_t) listener_thread;

NTSTATUS
NtSetEvent(void *handle, void *prev_state) {
  (void)prev_state;
  uintptr_t h = (uintptr_t)handle;
  if (h < 8)
    atomic_fetch_add(&set_count[h], 1);
  if (!pthread_equal(pthread_self(), atomic_load(&listener_thread)))
    atomic_fetch_add(&set_off_listener_thread, 1);
  return STATUS_SUCCESS;
}

#include LISTENER_SRC

static int failures = 0;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                       \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static const char *phase = "";

static void
on_alarm(int sig) {
  (void)sig;
  char msg[256];
  int n = snprintf(msg, sizeof(msg), "FAIL watchdog: %s hung (a listener join never returned)\n", phase);
  write(1, msg, n > 0 ? (size_t)n : 0);
  _exit(1);
}

static void
begin_phase(const char *name, unsigned seconds) {
  phase = name;
  alarm(seconds);
}

static obj_handle_t
listener_create(void) {
  struct unixcall_generic_obj_ret p = {0};
  _SharedEventListener_create(&p);
  return p.ret;
}

static void
listener_destroy(obj_handle_t h) {
  struct unixcall_generic_obj_noret p = {h};
  _SharedEventListener_destroy(&p);
}

static void *
listener_thread_main(void *arg) {
  atomic_store(&listener_thread, pthread_self());
  struct unixcall_generic_obj_noret p = {(obj_handle_t)(uintptr_t)arg};
  _SharedEventListener_start(&p);
  return NULL;
}

static pthread_t
listener_start_thread(obj_handle_t h) {
  pthread_t t;
  pthread_create(&t, NULL, listener_thread_main, (void *)(uintptr_t)h);
  return t;
}

static void
notify_at(obj_handle_t h, id<MTLSharedEvent> event, uintptr_t nt_handle, uint64_t value) {
  struct unixcall_mtlsharedevent_setevent p = {
      .shared_event = (obj_handle_t)event,
      .event_handle = (obj_handle_t)nt_handle,
      .shared_event_listener = h,
      .value = value,
  };
  _MTLSharedEvent_setWin32EventAtValue(&p);
}

static bool
wait_count(int idx, int want, int ms) {
  for (int i = 0; i < ms; i++) {
    if (atomic_load(&set_count[idx]) >= want)
      return true;
    usleep(1000);
  }
  return atomic_load(&set_count[idx]) >= want;
}

static void
spin_us(unsigned us) {
  uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + us * 1000ull;
  while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < end)
    ;
}

int
main(void) {
  signal(SIGALRM, on_alarm);
  setvbuf(stdout, NULL, _IONBF, 0);

  // 1. destroy before start
  begin_phase("part 1 (destroy before start)", 20);
  for (int i = 0; i < 300; i++) {
    obj_handle_t h = listener_create();
    listener_destroy(h);
    pthread_t t = listener_start_thread(h);
    pthread_join(t, NULL);
  }
  printf("ok part 1: destroy before start, 300 joins returned\n");

  // 2. destroy racing start
  begin_phase("part 2 (destroy racing start)", 60);
  srandom(1);
  for (int i = 0; i < 3000; i++) {
    obj_handle_t h = listener_create();
    pthread_t t = listener_start_thread(h);
    spin_us((unsigned)(random() % 200));
    listener_destroy(h);
    pthread_join(t, NULL);
  }
  printf("ok part 2: destroy racing start, 3000 joins returned\n");

  @autoreleasepool {
    id<MTLDevice> device = getenv("LISTENER_TEST_NO_DEVICE") ? nil : MTLCreateSystemDefaultDevice();
    if (!device) {
      // gw2 (#131): parts 3-5 didn't run, so this is no pass: 77 (skipped) unless parts 1-2 failed
      alarm(0);
      printf("skip parts 3-5: no Metal device; %s\n", failures ? "FAILED" : "SKIPPED (exit 77)");
      return failures ? 1 : 77;
    }

    // 3. delivery on the listener thread
    begin_phase("part 3 (delivery)", 20);
    {
      id<MTLSharedEvent> event = [device newSharedEvent];
      obj_handle_t h = listener_create();
      pthread_t t = listener_start_thread(h);
      for (uint64_t v = 1; v <= 5; v++) {
        notify_at(h, event, 1, v);
        event.signaledValue = v;
        CHECK(wait_count(1, (int)v, 2000), "value %llu: NtSetEvent called %d times", v,
              atomic_load(&set_count[1]));
      }
      listener_destroy(h);
      pthread_join(t, NULL);
      [event release];
      CHECK(atomic_load(&set_count[1]) == 5, "NtSetEvent called %d times for 5 values",
            atomic_load(&set_count[1]));
      CHECK(atomic_load(&set_off_listener_thread) == 0, "NtSetEvent ran %d times off the listener thread",
            atomic_load(&set_off_listener_thread));
    }
    printf("ok part 3: 5 notifications, NtSetEvent on the listener thread\n");

    // 4. a notification before start
    begin_phase("part 4 (notification before start)", 20);
    {
      id<MTLSharedEvent> event = [device newSharedEvent];
      obj_handle_t h = listener_create();
      notify_at(h, event, 2, 1);
      event.signaledValue = 1;
      usleep(20000); // the notification block now waits for the run loop
      CHECK(atomic_load(&set_count[2]) == 0, "NtSetEvent called before start");
      pthread_t t = listener_start_thread(h);
      CHECK(wait_count(2, 1, 2000), "the early notification was not delivered after start");
      listener_destroy(h);
      pthread_join(t, NULL);
      [event release];
    }
    printf("ok part 4: a notification before start is delivered once start runs\n");

    // 5. a notification pending at destroy
    begin_phase("part 5 (notification after destroy)", 20);
    {
      id<MTLSharedEvent> event = [device newSharedEvent];
      for (int i = 0; i < 50; i++) {
        obj_handle_t h = listener_create();
        pthread_t t = listener_start_thread(h);
        notify_at(h, event, 3, (uint64_t)i + 1);
        listener_destroy(h);
        pthread_join(t, NULL);
        event.signaledValue = (uint64_t)i + 1; // the block runs now, after destroy and the join
      }
      usleep(200000); // let every late block run (Metal's dispatch queue)
      [event release];
      CHECK(atomic_load(&set_count[3]) == 0, "NtSetEvent called %d times after destroy",
            atomic_load(&set_count[3]));
    }
    printf("ok part 5: 50 notifications after destroy dropped, no use after free\n");
    [device release];
  }

  alarm(0);
  printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
