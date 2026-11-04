// gw2-test: pure
// gw2-build: clang -std=c11 -O1 -Wall -Wextra -Werror -Isrc/winemetal/unix tests/gw2/pso_archive_policy_test.c src/winemetal/unix/pso_archive_policy.c -o $OUT
//
// gw2 (#75): the PSO archive's pure policy (src/winemetal/unix/pso_archive_policy.h): keep, skip
// and classify, under the legacy (#52) and the exact-shape policies. No Metal: records carry fake
// descriptors.

#include "pso_archive_policy.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                           \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static void
test_keyset(void) {
  struct pso_keyset s = {0};
  CHECK(!pso_keyset_has(&s, 5)); // empty, never initialized
  CHECK(pso_keyset_init(&s, 3));
  CHECK(s.mask + 1 == 64);
  for (uint64_t k = 1; k <= 1000; k++)
    CHECK(pso_keyset_insert(&s, k * 0x9e3779b97f4a7c15ull));
  CHECK(s.count == 1000);
  CHECK((s.count + 0) * 2 <= s.mask + 1); // grew, at most half full
  for (uint64_t k = 1; k <= 1000; k++)
    CHECK(pso_keyset_has(&s, k * 0x9e3779b97f4a7c15ull));
  CHECK(!pso_keyset_has(&s, 12345));
  CHECK(pso_keyset_insert(&s, 7) && pso_keyset_insert(&s, 7));
  CHECK(s.count == 1001); // a duplicate counts once
  // 0 is stored as 1 (pso_key never returns 0; the legacy known set did the same)
  CHECK(pso_keyset_insert(&s, 0));
  CHECK(pso_keyset_has(&s, 0) && pso_keyset_has(&s, 1));
  pso_keyset_free(&s);
  CHECK(!pso_keyset_has(&s, 7));
  // legacy sizing: the smallest power of two >= 64 and >= twice the keys
  CHECK(pso_keyset_init(&s, 100) && s.mask + 1 == 256);
  pso_keyset_free(&s);
}

static void
test_failed_index(void) {
  CHECK(pso_policy_failed_index("Failed to generate machO:error: cannot pack: expecting 'fragment' stage in pipeline "
                                "no. '3'") == 3);
  CHECK(pso_policy_failed_index("missing 'vertex' stage in pipeline no. '12'\n") == 12);
  CHECK(pso_policy_failed_index("rename failed") == -1);
  CHECK(pso_policy_failed_index("") == -1);
  CHECK(pso_policy_failed_index(NULL) == -1);
}

static void
push(struct pso_records *r, uint64_t key) {
  CHECK(pso_records_reserve(r));
  pso_records_push(r, key, (void *)(uintptr_t)(0x1000 + r->count), false);
}

static void
test_admit_and_shape(void) {
  struct pso_keyset known = {0}, prior = {0}, session = {0};
  pso_keyset_insert(&prior, 42);
  struct pso_policy legacy = {PSO_SKIP_RECORD, 2, &known, &prior};
  struct pso_policy shape = {PSO_SKIP_SHAPE, 2, &known, &prior};
  // the session cap: the third reservation is capped, as `g_reserved >= g_max_pipelines` was
  CHECK(pso_policy_admit(&legacy, 0) == PSO_KEEP);
  CHECK(pso_policy_admit(&legacy, 1) == PSO_KEEP);
  CHECK(pso_policy_admit(&legacy, 2) == PSO_KEEP_CAPPED);
  CHECK(pso_policy_admit(&shape, 5) == PSO_KEEP_CAPPED);
  // the legacy policy never excludes a shape, whatever the sets hold
  pso_keyset_insert(&session, 7);
  CHECK(pso_policy_shape(&legacy, &session, 42) == PSO_KEEP);
  CHECK(pso_policy_shape(&legacy, &session, 7) == PSO_KEEP);
  // exact shape: only the excluded keys themselves
  CHECK(pso_policy_shape(&shape, &session, 42) == PSO_KEEP_EXCLUDED);
  CHECK(pso_policy_shape(&shape, &session, 7) == PSO_KEEP_EXCLUDED);
  CHECK(pso_policy_shape(&shape, &session, 8) == PSO_KEEP);
  CHECK(pso_policy_shape(&shape, NULL, 7) == PSO_KEEP);
  pso_keyset_free(&known);
  pso_keyset_free(&prior);
  pso_keyset_free(&session);
}

static void
test_classify(void) {
  struct pso_keyset known = {0}, prior = {0};
  struct pso_policy legacy = {PSO_SKIP_RECORD, 8192, &known, &prior};
  CHECK(pso_policy_classify(&legacy, 1) == PSO_MISS_NEW); // no keys loaded
  pso_keyset_insert(&known, 10);
  pso_keyset_insert(&prior, 20);
  CHECK(pso_policy_classify(&legacy, 10) == PSO_MISS_KNOWN);
  CHECK(pso_policy_classify(&legacy, 20) == PSO_MISS_EXCLUDED);
  CHECK(pso_policy_classify(&legacy, 30) == PSO_MISS_NEW);
  // a key both persisted and excluded counts as known (it was archived)
  pso_keyset_insert(&known, 20);
  CHECK(pso_policy_classify(&legacy, 20) == PSO_MISS_KNOWN);
  struct pso_policy none = {PSO_SKIP_RECORD, 8192, NULL, NULL};
  CHECK(pso_policy_classify(&none, 10) == PSO_MISS_NEW);
  pso_keyset_free(&known);
  pso_keyset_free(&prior);
}

static void
test_skip_legacy(void) {
  struct pso_records r = {0};
  // keys: 100 101 102 100 (a second pipeline of key 100) 103
  push(&r, 100), push(&r, 101), push(&r, 102), push(&r, 100), push(&r, 103);
  struct pso_policy p = {PSO_SKIP_RECORD, 8192, NULL, NULL};
  struct pso_keyset session = {0};
  uint64_t newly = 99;
  CHECK(pso_policy_skip(&p, &r, r.map, r.map_count, -1, &session, &newly) == -1 && newly == 0);
  CHECK(pso_policy_skip(&p, &r, r.map, r.map_count, 5, &session, &newly) == -1 && newly == 0);
  // position 0 names record 0 only: its twin (record 3) stays, and nothing is excluded
  CHECK(pso_policy_skip(&p, &r, r.map, r.map_count, 0, &session, &newly) == 0 && newly == 1);
  CHECK(r.rec[0].skipped && !r.rec[3].skipped && !r.rec[1].skipped);
  CHECK(session.count == 0);
  // naming it again marks nothing new (#52 counted `skipped` once)
  CHECK(pso_policy_skip(&p, &r, r.map, r.map_count, 0, &session, &newly) == 0 && newly == 0);
  // a rebuilt archive's order (records 1 2 3 4): its position 2 is record 3
  uint64_t map[] = {1, 2, 3, 4};
  CHECK(pso_policy_skip(&p, &r, map, 4, 2, &session, &newly) == 3 && newly == 1 && r.rec[3].skipped);
  uint64_t keys[5];
  CHECK(pso_records_kept_keys(&r, 0, keys) == 3 && keys[0] == 101 && keys[1] == 102 && keys[2] == 103);
  CHECK(pso_records_kept_keys(&r, 2, NULL) == 2);
  pso_records_free(&r);
  pso_keyset_free(&session);
}

static void
test_skip_exact_shape(void) {
  struct pso_records r = {0};
  push(&r, 100), push(&r, 101), push(&r, 100), push(&r, 102);
  // fragment-less or not, the policy never sees it: only what Metal named, by its full key
  struct pso_keyset session = {0};
  struct pso_policy p = {PSO_SKIP_SHAPE, 8192, NULL, NULL};
  uint64_t newly = 0;
  CHECK(pso_policy_skip(&p, &r, r.map, r.map_count, 2, &session, &newly) == 2);
  CHECK(newly == 2 && r.rec[0].skipped && r.rec[2].skipped);
  CHECK(!r.rec[1].skipped && !r.rec[3].skipped); // no class skip: other keys stay
  CHECK(pso_keyset_has(&session, 100) && session.count == 1);
  CHECK(pso_policy_shape(&p, &session, 100) == PSO_KEEP_EXCLUDED);
  CHECK(pso_policy_shape(&p, &session, 101) == PSO_KEEP);
  CHECK(pso_policy_skip(&p, &r, r.map, r.map_count, 0, &session, &newly) == 0 && newly == 0);
  pso_records_free(&r);
  pso_keyset_free(&session);
}

static void
test_records_growth(void) {
  struct pso_records r = {0};
  for (uint64_t i = 0; i < 1000; i++)
    push(&r, i + 1);
  CHECK(r.count == 1000 && r.map_count == 1000 && r.cap == 1024 && r.map_cap == 1024);
  CHECK(r.map[999] == 999 && r.rec[999].key == 1000 && r.rec[999].desc == (void *)(uintptr_t)(0x1000 + 999));
  uint64_t *map = NULL, count = 0, cap = 0;
  for (uint64_t i = 0; i < 300; i++)
    CHECK(pso_records_order_push(&map, &count, &cap, i * 2));
  CHECK(count == 300 && cap == 512 && map[299] == 598);
  free(map);
  pso_records_free(&r);
}

int
main(void) {
  test_keyset();
  test_failed_index();
  test_admit_and_shape();
  test_classify();
  test_skip_legacy();
  test_skip_exact_shape();
  test_records_growth();
  if (failures) {
    printf("%d failures\n", failures);
    return 1;
  }
  printf("pso_archive_policy_test: ok\n");
  return 0;
}
