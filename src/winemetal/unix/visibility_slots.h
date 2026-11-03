/* gw2 (#1, #20): the cached visibility of metal views, for MetalView_visibility (bridge call 147).

   A view owns a slot from its first visibility call until ReleaseMetalView. A call returns the
   slot's cached flags and asks for at most one asynchronous refresh at a time; the refresh reads
   AppKit on the main thread and publishes its flags afterwards.

   gw2 (#20): one lock covers a slot's owner, flags and pending refresh, and every claim and release
   bumps the slot's generation. A refresh publishes (and clears the pending mark) only if the slot
   still has the generation it was queued under, so a refresh still in flight when its view is
   released never writes into the slot's next owner, even one at the same address. The lock is held
   only for a few loads and stores, never across AppKit work, so the render thread never waits for
   the main thread's refresh itself.

   Pure C with no AppKit, so tests/gw2/visibility_slots_test.c can stress it on the host. */
#pragma once

#include <os/lock.h>
#include <stdint.h>

#define WMT_VISIBILITY_SLOTS 8

struct wmt_visibility_slot {
  const void *view;    /* the owner, NULL while free */
  uint64_t generation; /* bumped on every claim and release */
  uint64_t flags;      /* the owner's last published flags; 0 = not known yet */
  int pending;         /* a refresh is queued for this generation */
};

struct wmt_visibility_table {
  os_unfair_lock lock;
  struct wmt_visibility_slot slots[WMT_VISIBILITY_SLOTS];
};

#define WMT_VISIBILITY_TABLE_INIT {OS_UNFAIR_LOCK_INIT, {{0}}}

/* What a visibility call does next: return `flags`; if `refresh`, queue one refresh that publishes
   with `slot` and `generation`. `slot` < 0: no slot (more live views than slots), flags unknown. */
struct wmt_visibility_lookup {
  int slot;
  int refresh;
  uint64_t generation;
  uint64_t flags;
};

static inline struct wmt_visibility_lookup
wmt_visibility_lookup(struct wmt_visibility_table *t, const void *view) {
  struct wmt_visibility_lookup r = {-1, 0, 0, 0};
  os_unfair_lock_lock(&t->lock);
  for (int i = 0; i < WMT_VISIBILITY_SLOTS && r.slot < 0; i++)
    if (t->slots[i].view == view)
      r.slot = i;
  for (int i = 0; i < WMT_VISIBILITY_SLOTS && r.slot < 0; i++) {
    struct wmt_visibility_slot *s = &t->slots[i];
    if (!s->view) {
      s->view = view;
      s->generation++;
      s->flags = 0;
      s->pending = 0;
      r.slot = i;
    }
  }
  if (r.slot >= 0) {
    struct wmt_visibility_slot *s = &t->slots[r.slot];
    r.flags = s->flags;
    r.generation = s->generation;
    r.refresh = !s->pending;
    s->pending = 1;
  }
  os_unfair_lock_unlock(&t->lock);
  return r;
}

/* The refresh's result. Returns 0 (and changes nothing) if the slot was released or reused since
   the refresh was queued. */
static inline int
wmt_visibility_publish(struct wmt_visibility_table *t, int slot, uint64_t generation,
                       uint64_t flags) {
  int published = 0;
  os_unfair_lock_lock(&t->lock);
  struct wmt_visibility_slot *s = &t->slots[slot];
  if (s->generation == generation) {
    s->flags = flags;
    s->pending = 0;
    published = 1;
  }
  os_unfair_lock_unlock(&t->lock);
  return published;
}

static inline void
wmt_visibility_release(struct wmt_visibility_table *t, const void *view) {
  if (!view)
    return;
  os_unfair_lock_lock(&t->lock);
  for (int i = 0; i < WMT_VISIBILITY_SLOTS; i++) {
    struct wmt_visibility_slot *s = &t->slots[i];
    if (s->view == view) {
      s->view = NULL;
      s->generation++;
      s->flags = 0;
      s->pending = 0;
    }
  }
  os_unfair_lock_unlock(&t->lock);
}
