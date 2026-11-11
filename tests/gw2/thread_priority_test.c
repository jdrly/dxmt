// gw2-test: pure
// gw2-build: U=($(sed -n "/^winemetal_unix_src = files(/,/^)/s/^ *'\([a-z0-9_]*\.c\)'.*/\1/p" src/winemetal/unix/meson.build))
// gw2-build: (( ${#U} >= 5 ))
// gw2-build: for arch in x86_64 arm64; do
// gw2-build:   for f in $U; do clang -c -x objective-c -std=gnu17 -fblocks -fno-objc-arc -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Wno-unused-function -Isrc/airconv -Isrc/nativemetal -Isrc/winemetal src/winemetal/unix/$f -o $T/tp_${f:r}_$arch.o; done
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal src/winemetal/winemetal_thunks.c -o $T/tp_thunks_$arch.o
// gw2-build:   clang -c -std=gnu17 -arch $arch -mmacosx-version-min=26.0 -Wno-ignored-attributes -Isrc/airconv tests/gw2/shim/airconv_absent.c -o $T/tp_airconv_absent_$arch.o
// gw2-build:   clang -std=gnu17 -Wall -Wextra -Werror -arch $arch -mmacosx-version-min=26.0 -DDXMT_NATIVE=1 -Isrc/nativemetal -Isrc/winemetal tests/gw2/thread_priority_test.c $T/tp_*_$arch.o -framework Metal -framework MetalFX -framework QuartzCore -framework Foundation -framework CoreGraphics -framework ColorSync -framework Cocoa -lsqlite3 -lobjc -o ${OUT}_$arch
// gw2-build: done
// gw2-run: ${OUT}_x86_64 && ${OUT}_arm64
//
// gw2 (#151): bridge call WMTSetThreadBasePrioritySelf, called through the PE thunk and the production
// table (wmt_native_dispatch, ADR 0005), on threads in the states the wineserver leaves a Wine thread
// in: Mach policies set (so no QoS class), timeshare on, precedence importance 0 (THREAD_PRIORITY_NORMAL,
// which DXMT_THREAD_PRIORITY=utility sets first) or 9 (HIGHEST), or timeshare off and importance 32
// (TIME_CRITICAL). Each must read base priority 20 afterwards, read independently of the call.
//
// The main thread never pthread_joins a thread before that thread has measured: a joiner lends the
// joined thread its QoS as an override, which raises its base priority to 31 while the join lasts.
// Needs no Metal device, only the bridge's unix side linked in. Skips (77) when the thread's maximum
// priority is 20 or less: GitHub's macOS runner runs its jobs clamped to the utility band (task and
// maximum priority 20, as `taskpolicy -c utility` does), where no thread can go above or below 20.
// A task that is only niced to 20 keeps maximum priority 63 and runs the test.

#include <libproc.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/proc_info.h>
#include <unistd.h>
#include "winemetal.h"

static int failures;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static int
thread_info_self(struct proc_threadinfo *info) {
  uint64_t tid;
  return !pthread_threadid_np(NULL, &tid) &&
         proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, tid, info, sizeof(*info)) == sizeof(*info);
}

static int
base_priority_self(void) {
  struct proc_threadinfo info;
  return thread_info_self(&info) ? info.pth_priority : -1;
}

struct wine_state {
  const char *name;
  int timeshare;
  int importance;
};

struct result {
  int policy_ret, ret, old_base, new_base, read_base, wine_base;
  atomic_int done;
};

static struct wine_state current_state;
static struct result current_result;

static void *
thread_main(void *unused) {
  (void)unused;
  struct wine_state *s = &current_state;
  struct result *r = &current_result;
  mach_port_t port = pthread_mach_thread_np(pthread_self());
  // what the wineserver's apply_thread_priority does to the thread
  struct thread_extended_policy extended = {.timeshare = s->timeshare};
  struct thread_precedence_policy precedence = {.importance = s->importance};
  r->policy_ret = thread_policy_set(port, THREAD_EXTENDED_POLICY, (thread_policy_t)&extended, THREAD_EXTENDED_POLICY_COUNT) |
                  thread_policy_set(port, THREAD_PRECEDENCE_POLICY, (thread_policy_t)&precedence, THREAD_PRECEDENCE_POLICY_COUNT);
  r->wine_base = base_priority_self();
  r->ret = WMTSetThreadBasePrioritySelf(20, &r->old_base, &r->new_base);
  r->read_base = base_priority_self();
  atomic_store(&r->done, 1);
  return NULL;
}

int
main(void) {
  struct proc_taskinfo task;
  struct proc_threadinfo self;
  if (proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &task, sizeof(task)) != sizeof(task) || !thread_info_self(&self)) {
    printf("FAIL: can't read the task's priority or this thread's maximum priority\n");
    return 1;
  }
  if (self.pth_maxpriority <= 20) {
    printf("skip: maximum priority %d (task priority %d); a task clamped to the utility band or below (a CI "
           "runner's) keeps every thread at or under it, so base priority 20 can't be told apart\n",
           self.pth_maxpriority, task.pti_priority);
    return 77;
  }
  const struct wine_state states[] = {
      {"normal (timeshare, importance 0)", 1, 0},
      {"highest (timeshare, importance 9)", 1, 9},
      {"time-critical (fixed, importance 32)", 0, 32},
  };
  for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
    current_state = states[i];
    current_result = (struct result){0};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, thread_main, NULL) == 0);
    while (!atomic_load(&current_result.done))
      usleep(1000); // not pthread_join: see the header
    pthread_join(thread, NULL);
    struct result *r = &current_result;
    printf("%s: task priority %d, Wine-like base %d, call %d, base %d -> %d, read %d\n", states[i].name,
           task.pti_priority, r->wine_base, r->ret, r->old_base, r->new_base, r->read_base);
    CHECK(r->policy_ret == 0);
    int expected = task.pti_priority + states[i].importance;
    CHECK(r->wine_base == (expected > self.pth_maxpriority ? self.pth_maxpriority : expected));
    CHECK(r->ret == 0);
    CHECK(r->old_base == r->wine_base);
    CHECK(r->new_base == 20);
    CHECK(r->read_base == 20);
  }
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("thread priority: base priority 20 from 3 Wine-like states: OK\n");
  return 0;
}
