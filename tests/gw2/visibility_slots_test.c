// gw2 (#20): host-side test of the visibility slots behind MetalView_visibility (bridge call 147,
// src/winemetal/unix/visibility_slots.h). A serial dispatch queue stands in for the main thread.
// Not part of the build; run it on the Mac:
//
//   clang -std=c11 -O2 -Isrc/winemetal/unix tests/gw2/visibility_slots_test.c \
//     -o /tmp/visibility_slots_test && /tmp/visibility_slots_test

#include "visibility_slots.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static atomic_int failures;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
      atomic_fetch_add(&failures, 1);                                                              \
    }                                                                                              \
  } while (0)

// The race of the review of PR #15, step by step: view A queues a refresh, A is released, view B
// (here at A's address, the worst case) claims the same slot, then A's refresh lands.
static void
reuse_while_refresh_in_flight(void) {
  struct wmt_visibility_table t = WMT_VISIBILITY_TABLE_INIT;
  static char view;

  struct wmt_visibility_lookup a = wmt_visibility_lookup(&t, &view);
  CHECK(a.slot == 0 && a.refresh && a.flags == 0);
  wmt_visibility_release(&t, &view);

  struct wmt_visibility_lookup b = wmt_visibility_lookup(&t, &view);
  CHECK(b.slot == 0 && b.refresh && b.flags == 0);
  CHECK(b.generation != a.generation);

  // A's refresh (occluded) lands: dropped, and B's pending refresh stays pending.
  CHECK(!wmt_visibility_publish(&t, a.slot, a.generation, 0x3));
  struct wmt_visibility_lookup b2 = wmt_visibility_lookup(&t, &view);
  CHECK(b2.flags == 0);
  CHECK(!b2.refresh); // one refresh at a time: B's first is still queued

  // B's refresh lands.
  CHECK(wmt_visibility_publish(&t, b.slot, b.generation, 0x5));
  struct wmt_visibility_lookup b3 = wmt_visibility_lookup(&t, &view);
  CHECK(b3.flags == 0x5 && b3.refresh);
}

// A refresh that lands after release, with the slot still free, leaves it free and clean.
static void
publish_after_release(void) {
  struct wmt_visibility_table t = WMT_VISIBILITY_TABLE_INIT;
  static char a, b;
  struct wmt_visibility_lookup ra = wmt_visibility_lookup(&t, &a);
  wmt_visibility_release(&t, &a);
  CHECK(!wmt_visibility_publish(&t, ra.slot, ra.generation, 0x3));
  CHECK(t.slots[ra.slot].view == NULL && t.slots[ra.slot].flags == 0);
  struct wmt_visibility_lookup rb = wmt_visibility_lookup(&t, &b);
  CHECK(rb.slot == ra.slot && rb.flags == 0 && rb.refresh);
}

// More views than slots: the extra view gets no slot until one is released.
static void
full_table(void) {
  struct wmt_visibility_table t = WMT_VISIBILITY_TABLE_INIT;
  static char views[WMT_VISIBILITY_SLOTS + 1];
  for (int i = 0; i < WMT_VISIBILITY_SLOTS; i++)
    CHECK(wmt_visibility_lookup(&t, &views[i]).slot == i);
  CHECK(wmt_visibility_lookup(&t, &views[WMT_VISIBILITY_SLOTS]).slot < 0);
  wmt_visibility_release(&t, &views[3]);
  CHECK(wmt_visibility_lookup(&t, &views[WMT_VISIBILITY_SLOTS]).slot == 3);
}

// Stress: more render threads than slots, each going through many view lifetimes. Half the
// threads reuse one address for all their lifetimes; the other half get a new address each time.
// A refresh publishes flags that name its lifetime, so a thread must only ever see flags of its
// current lifetime (or 0), and never more than one unpublished refresh of a lifetime.
#define THREADS 12
#define LIFETIMES 3000
#define CALLS 40

static struct wmt_visibility_table g_table = WMT_VISIBILITY_TABLE_INIT;
static dispatch_queue_t g_main;

struct worker {
  int id;
  atomic_int outstanding[LIFETIMES]; // refreshes in flight, per lifetime
  atomic_int stale;                  // flags of another lifetime seen
  atomic_int known;                  // lifetimes that saw their own flags
};

static uint64_t
lifetime_flags(int id, int lifetime) {
  return ((uint64_t)id << 40) | ((uint64_t)(lifetime + 1) << 8) | 1;
}

static void *
render_thread(void *arg) {
  struct worker *w = arg;
  char *views = calloc(LIFETIMES, 1);
  for (int lt = 0; lt < LIFETIMES; lt++) {
    const void *view = (w->id & 1) ? (const void *)w : (const void *)&views[lt];
    int saw_own = 0;
    // every 10th lifetime waits (up to about 2 s) for its own flags: no slot stays pending forever
    int waits = lt % 10 == 0;
    for (int c = 0; c < CALLS || (waits && !saw_own && c < 200000); c++) {
      if (c >= CALLS)
        usleep(10);
      struct wmt_visibility_lookup r = wmt_visibility_lookup(&g_table, view);
      if (r.slot < 0)
        continue;
      if (r.flags) {
        if (r.flags == lifetime_flags(w->id, lt))
          saw_own = 1;
        else
          atomic_fetch_add(&w->stale, 1);
      }
      if (r.refresh) {
        if (atomic_fetch_add(&w->outstanding[lt], 1) != 0)
          CHECK(!"two refreshes of one lifetime in flight");
        int slot = r.slot, lifetime = lt;
        uint64_t generation = r.generation;
        dispatch_async(g_main, ^{
          if (lifetime % 7 == 0)
            usleep(20); // widen the window between the lookup and the publish
          // done before it publishes: once published, the next refresh may be queued
          atomic_fetch_sub(&w->outstanding[lifetime], 1);
          wmt_visibility_publish(&g_table, slot, generation, lifetime_flags(w->id, lifetime));
        });
      }
      if (c % 8 == 7)
        sched_yield();
    }
    if (saw_own)
      atomic_fetch_add(&w->known, 1);
    else if (waits)
      CHECK(!"a waiting lifetime never saw its own flags");
    wmt_visibility_release(&g_table, view);
  }
  free(views);
  return NULL;
}

static void
stress(void) {
  g_main = dispatch_queue_create("visibility_slots_test.main", DISPATCH_QUEUE_SERIAL);
  struct worker *workers = calloc(THREADS, sizeof(*workers));
  pthread_t threads[THREADS];
  for (int i = 0; i < THREADS; i++) {
    workers[i].id = i + 1;
    pthread_create(&threads[i], NULL, render_thread, &workers[i]);
  }
  for (int i = 0; i < THREADS; i++)
    pthread_join(threads[i], NULL);
  dispatch_sync(g_main, ^{});

  int stale = 0, known = 0;
  for (int i = 0; i < THREADS; i++) {
    stale += atomic_load(&workers[i].stale);
    known += atomic_load(&workers[i].known);
  }
  printf("stress: %d threads, %d slots, %d lifetimes x %d calls each: %d stale reads, "
         "%d of %d lifetimes saw their own flags\n",
         THREADS, WMT_VISIBILITY_SLOTS, LIFETIMES, CALLS, stale, known, THREADS * LIFETIMES);
  CHECK(stale == 0);
  CHECK(known > 0); // the refreshes did land
  for (int i = 0; i < WMT_VISIBILITY_SLOTS; i++)
    CHECK(g_table.slots[i].view == NULL && g_table.slots[i].flags == 0 &&
          g_table.slots[i].pending == 0);
  free(workers);
}

int
main(void) {
  reuse_while_refresh_in_flight();
  publish_after_release();
  full_table();
  stress();
  int f = atomic_load(&failures);
  printf("%s (%d failures)\n", f ? "FAILED" : "ok", f);
  return f ? 1 : 0;
}
