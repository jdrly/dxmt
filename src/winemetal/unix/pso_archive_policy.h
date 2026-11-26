#pragma once

/* gw2 (#75): the PSO archive's policy, pure (plain C, no Metal or Foundation types).
 *
 * What the archive keeps, what a failed flush leaves out, and how a lookup miss is classified are
 * decided here, from keys, record indices and the packer's error text only. The Metal 3 adapter
 * (pso_archive.c, MTLBinaryArchive), the native archive tool and #34's MTL4Archive adapter share it.
 *
 * - keep: a built pipeline is captured unless the session reached its cap or (exact-shape policy)
 *   its key is a failing shape;
 * - skip: a failed flush leaves out the record Metal's packer names ("pipeline no. 'N'", in the
 *   archive's own order). The legacy policy (#52) marks that one record; the exact-shape policy
 *   marks every record of the same full key and excludes the key. Neither ever skips a class of
 *   pipelines (fragment-less or any other): only what Metal named;
 * - classify: a lookup miss is `known` (a loaded archive's persisted keys hold it, yet Metal
 *   missed), `excluded` (a failing shape an earlier generation recorded) or `new`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A set of pipeline keys (open addressing, 0 = empty: a key of 0 is stored as 1). */
struct pso_keyset {
  uint64_t *slots;
  uint64_t mask;
  uint64_t count;
};

/* Sized for `expected` keys (the table is at least twice that, at least 64); false if out of memory
   (the set stays empty and usable: it holds nothing). */
bool pso_keyset_init(struct pso_keyset *set, uint64_t expected);
/* Adds `key`, growing the table when it is half full; false if out of memory (not added). */
bool pso_keyset_insert(struct pso_keyset *set, uint64_t key);
bool pso_keyset_has(const struct pso_keyset *set, uint64_t key);
void pso_keyset_free(struct pso_keyset *set);

/* One pipeline added to a session's archive. `desc` is the adapter's (a retained Metal descriptor
   in pso_archive.c); the policy never looks at it. */
struct pso_record {
  uint64_t key;
  void *desc;
  bool compute;
  bool skipped; /* left out of the archive after Metal's packer named it */
};

/* A session's records in add order, and the current archive's order (what Metal's "pipeline no. N"
   counts) as record indices. */
struct pso_records {
  struct pso_record *rec;
  uint64_t count, cap;
  uint64_t *map;
  uint64_t map_count, map_cap;
};

/* Room for one more record and one more map entry (so a record is never added without its place in
   the order); false (unchanged) if out of memory. */
bool pso_records_reserve(struct pso_records *r);
/* Appends a record and its place in the current order (after pso_records_reserve). */
void pso_records_push(struct pso_records *r, uint64_t key, void *desc, bool compute);
/* Appends record index `rec` to an archive's order (a rebuilt archive's own map); false if out of
   memory. */
bool pso_records_order_push(uint64_t **map, uint64_t *count, uint64_t *cap, uint64_t rec);
/* The keys of the records from `from` on that are not skipped, in add order (duplicates included),
   into `out` if not NULL; their number. What a successful write persists. */
uint64_t pso_records_kept_keys(const struct pso_records *r, uint64_t from, uint64_t *out);
void pso_records_free(struct pso_records *r); /* the descs are the caller's */

enum pso_failure_policy {
  PSO_SKIP_RECORD = 0, /* legacy (#52): the named record only */
  PSO_SKIP_SHAPE = 1,  /* every record of the named record's key; the key is excluded from capture */
};

struct pso_policy {
  enum pso_failure_policy failure;
  uint32_t max_pipelines;                 /* per session */
  const struct pso_keyset *known;         /* persisted keys of the loaded archives; immutable */
  const struct pso_keyset *excluded_prior; /* failing shapes earlier generations recorded; immutable */
};

enum pso_keep {
  PSO_KEEP = 0,
  PSO_KEEP_CAPPED,   /* the session reached max_pipelines */
  PSO_KEEP_EXCLUDED, /* a failing shape (exact-shape policy only) */
};

/* At build time: `reserved_before` is the number of captures reserved before this one. */
enum pso_keep pso_policy_admit(const struct pso_policy *p, uint64_t reserved_before);
/* At add time (under the archive's lock): whether `key` is a failing shape, earlier or this
   session's (`excluded_session`, may be NULL). Always PSO_KEEP under the legacy policy. */
enum pso_keep pso_policy_shape(const struct pso_policy *p, const struct pso_keyset *excluded_session,
                               uint64_t key);

enum pso_miss {
  PSO_MISS_NEW = 0,
  PSO_MISS_KNOWN,
  PSO_MISS_EXCLUDED,
};

/* A lookup miss of `key` (lock-free: reads only the immutable sets). */
enum pso_miss pso_policy_classify(const struct pso_policy *p, uint64_t key);

/* The index Metal's packer names in a failed serialization ("pipeline no. 'N'"); -1 if none. */
long pso_policy_failed_index(const char *why);

/* After a failed write that named position `idx` of an archive's order `map`: marks what the policy
   leaves out. Returns the named record's index, or -1 if `idx` isn't a recorded pipeline (nothing
   marked). `*newly` is the number of records newly marked. The exact-shape policy also adds the
   key to `excluded_session`. */
int64_t pso_policy_skip(const struct pso_policy *p, struct pso_records *r, const uint64_t *map, uint64_t map_count,
                        long idx, struct pso_keyset *excluded_session, uint64_t *newly);
