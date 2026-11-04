/* gw2 (#75): the PSO archive's pure policy. See pso_archive_policy.h. */

#include "pso_archive_policy.h"
#include <stdlib.h>
#include <string.h>

static uint64_t
keyset_norm(uint64_t key) {
  return key ? key : 1;
}

bool
pso_keyset_init(struct pso_keyset *set, uint64_t expected) {
  memset(set, 0, sizeof(*set));
  uint64_t cap = 64;
  while (cap < expected * 2)
    cap <<= 1;
  set->slots = calloc(cap, sizeof(uint64_t));
  if (!set->slots)
    return false;
  set->mask = cap - 1;
  return true;
}

static void
keyset_place(uint64_t *slots, uint64_t mask, uint64_t key, uint64_t *count) {
  for (uint64_t i = key & mask;; i = (i + 1) & mask) {
    if (slots[i] == key)
      return;
    if (!slots[i]) {
      slots[i] = key;
      ++*count;
      return;
    }
  }
}

bool
pso_keyset_insert(struct pso_keyset *set, uint64_t key) {
  key = keyset_norm(key);
  if (!set->slots && !pso_keyset_init(set, 32))
    return false;
  if (pso_keyset_has(set, key))
    return true;
  if ((set->count + 1) * 2 > set->mask + 1) {
    uint64_t cap = (set->mask + 1) * 2;
    uint64_t *grown = calloc(cap, sizeof(uint64_t));
    if (!grown)
      return false;
    uint64_t n = 0;
    for (uint64_t i = 0; i <= set->mask; i++)
      if (set->slots[i])
        keyset_place(grown, cap - 1, set->slots[i], &n);
    free(set->slots);
    set->slots = grown;
    set->mask = cap - 1;
    set->count = n;
  }
  keyset_place(set->slots, set->mask, key, &set->count);
  return true;
}

bool
pso_keyset_has(const struct pso_keyset *set, uint64_t key) {
  if (!set || !set->slots)
    return false;
  key = keyset_norm(key);
  for (uint64_t i = key & set->mask;; i = (i + 1) & set->mask) {
    if (set->slots[i] == key)
      return true;
    if (!set->slots[i])
      return false;
  }
}

void
pso_keyset_free(struct pso_keyset *set) {
  free(set->slots);
  memset(set, 0, sizeof(*set));
}

/* grows a buffer of `elem`-byte items to hold at least `need`; false (unchanged) if it can't */
static bool
grow(void **buf, uint64_t *cap, uint64_t need, size_t elem) {
  if (need <= *cap)
    return true;
  uint64_t ncap = *cap ? *cap * 2 : 256;
  while (ncap < need)
    ncap *= 2;
  void *grown = realloc(*buf, ncap * elem);
  if (!grown)
    return false;
  *buf = grown;
  *cap = ncap;
  return true;
}

bool
pso_records_reserve(struct pso_records *r) {
  return grow((void **)&r->rec, &r->cap, r->count + 1, sizeof(*r->rec)) &&
         grow((void **)&r->map, &r->map_cap, r->map_count + 1, sizeof(*r->map));
}

void
pso_records_push(struct pso_records *r, uint64_t key, void *desc, bool compute) {
  r->rec[r->count].key = key;
  r->rec[r->count].desc = desc;
  r->rec[r->count].compute = compute;
  r->rec[r->count].skipped = false;
  r->map[r->map_count++] = r->count;
  r->count++;
}

bool
pso_records_order_push(uint64_t **map, uint64_t *count, uint64_t *cap, uint64_t rec) {
  if (!grow((void **)map, cap, *count + 1, sizeof(**map)))
    return false;
  (*map)[(*count)++] = rec;
  return true;
}

uint64_t
pso_records_kept_keys(const struct pso_records *r, uint64_t from, uint64_t *out) {
  uint64_t n = 0;
  for (uint64_t i = from; i < r->count; i++)
    if (!r->rec[i].skipped) {
      if (out)
        out[n] = r->rec[i].key;
      n++;
    }
  return n;
}

void
pso_records_free(struct pso_records *r) {
  free(r->rec);
  free(r->map);
  memset(r, 0, sizeof(*r));
}

enum pso_keep
pso_policy_admit(const struct pso_policy *p, uint64_t reserved_before) {
  return reserved_before >= p->max_pipelines ? PSO_KEEP_CAPPED : PSO_KEEP;
}

enum pso_keep
pso_policy_shape(const struct pso_policy *p, const struct pso_keyset *excluded_session, uint64_t key) {
  if (p->failure != PSO_SKIP_SHAPE)
    return PSO_KEEP;
  if (pso_keyset_has(p->excluded_prior, key) || pso_keyset_has(excluded_session, key))
    return PSO_KEEP_EXCLUDED;
  return PSO_KEEP;
}

enum pso_miss
pso_policy_classify(const struct pso_policy *p, uint64_t key) {
  if (pso_keyset_has(p->known, key))
    return PSO_MISS_KNOWN;
  if (pso_keyset_has(p->excluded_prior, key))
    return PSO_MISS_EXCLUDED;
  return PSO_MISS_NEW;
}

long
pso_policy_failed_index(const char *why) {
  const char *p = why ? strstr(why, "pipeline no. '") : NULL;
  return p ? strtol(p + 14, NULL, 10) : -1;
}

int64_t
pso_policy_skip(const struct pso_policy *p, struct pso_records *r, const uint64_t *map, uint64_t map_count, long idx,
                struct pso_keyset *excluded_session, uint64_t *newly) {
  *newly = 0;
  if (idx < 0 || (uint64_t)idx >= map_count)
    return -1;
  uint64_t named = map[idx];
  struct pso_record *rec = &r->rec[named];
  if (p->failure == PSO_SKIP_SHAPE) {
    /* exact shape: the same full key, nothing broader */
    uint64_t key = rec->key;
    for (uint64_t i = 0; i < r->count; i++)
      if (r->rec[i].key == key && !r->rec[i].skipped) {
        r->rec[i].skipped = true;
        ++*newly;
      }
    if (excluded_session)
      pso_keyset_insert(excluded_session, key);
  } else if (!rec->skipped) {
    rec->skipped = true;
    *newly = 1;
  }
  return (int64_t)named;
}
