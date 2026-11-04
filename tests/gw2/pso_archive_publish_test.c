// gw2-test: pure
// gw2-build: clang -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/winemetal/unix tests/gw2/pso_archive_publish_test.c src/winemetal/unix/pso_archive_publish.c -o $OUT
// gw2-run: "$OUT" "$T/pso_archive_publish"
//
// gw2 (#75): the PSO archive's generation publisher (src/winemetal/unix/pso_archive_publish.h) with a
// fake shard writer (no Metal): round trips, a failure at every publish step leaving the last good
// generation, corrupt shards, manifests and index, the writer lock, collection and session
// retention, and exclusion-only generations. Works in the directory given (made empty first).

#include "pso_archive_publish.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                           \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static char g_dir[1024];
static char g_report[16384];

static struct pso_report
report(void) {
  g_report[0] = '\0';
  struct pso_report r = {g_report, sizeof(g_report), 0};
  return r;
}

struct fake {
  const char *content;
  const char *fail; // the callback fails with this text
};

static const char *
fake_write(void *ctx, const char *path, char *why, size_t why_size) {
  struct fake *f = ctx;
  if (f->fail) {
    snprintf(why, why_size, "%s", f->fail);
    return why;
  }
  FILE *fp = fopen(path, "wb");
  if (!fp)
    return "fopen";
  fputs(f->content, fp);
  fclose(fp);
  return NULL;
}

static void
open_store(struct pso_gen_store *s, uint64_t started, uint64_t pid, uint32_t keep) {
  struct pso_report r = report();
  CHECK(pso_gen_open(s, g_dir, keep, started, pid, &r));
}

static const char *
publish(struct pso_gen_store *s, const char *content, const uint64_t *keys, uint64_t n, bool replace) {
  static char why[512];
  struct fake f = {content, NULL};
  uint64_t bytes = 0;
  const char *failed = pso_gen_publish(s, fake_write, &f, keys, n, replace, NULL, 0, &bytes, why, sizeof(why));
  if (!failed)
    CHECK(bytes == strlen(content));
  return failed;
}

static unsigned
count_files(const char *suffix) {
  DIR *d = opendir(g_dir);
  unsigned n = 0;
  struct dirent *e;
  while (d && (e = readdir(d))) {
    size_t l = strlen(e->d_name), k = strlen(suffix);
    n += l >= k && !strcmp(e->d_name + l - k, suffix);
  }
  if (d)
    closedir(d);
  return n;
}

static void
reset_dir(void) {
  char cmd[1200];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
  CHECK(!system(cmd));
}

static void
test_round_trip(void) {
  reset_dir();
  struct pso_gen_store s = {0};
  open_store(&s, 100, 1, 4);
  CHECK(s.role == PSO_GEN_WRITER && s.current.generation == 0 && s.next_generation == 1);
  uint64_t k1[] = {11, 12, 13};
  CHECK(!publish(&s, "shard one", k1, 3, false));
  CHECK(s.current.generation == 1 && s.current.shard_count == 1 && s.current.shards[0].key_count == 3);
  uint64_t k2[] = {14};
  CHECK(!publish(&s, "shard two", k2, 1, false));
  CHECK(s.current.generation == 2 && s.current.prev == 1 && s.current.shard_count == 2);
  CHECK(pso_gen_session_shards(&s) == 2);
  pso_gen_close(&s);

  struct pso_gen_store t = {0};
  open_store(&t, 200, 2, 4);
  CHECK(t.current.generation == 2 && t.current.shard_count == 2 && t.next_generation == 3);
  uint64_t total = 0;
  for (uint64_t i = 0; i < t.current.shard_count; i++)
    total += t.current.shards[i].key_count;
  CHECK(total == 4);
  CHECK(pso_gen_session_shards(&t) == 0);
  // a merge replaces only this session's shards; the other session's stay
  CHECK(!publish(&t, "t delta", k2, 1, false));
  CHECK(!publish(&t, "t merged", k1, 3, true));
  CHECK(t.current.shard_count == 3 && pso_gen_session_shards(&t) == 1);
  CHECK(t.current.shards[0].started == 200 && t.current.shards[0].key_count == 3);
  // only the current and the previous generation stay on disk
  CHECK(count_files(".manifest") == 2);
  // shards: gen 4 (s's two + t merged) and gen 3 (s's two + t delta) -> 4 files
  CHECK(count_files(".shard") == 4);
  pso_gen_close(&t);
}

static void
test_fault_every_step(void) {
  for (int step = 1; step <= 7; step++) {
    reset_dir();
    struct pso_gen_store s = {0};
    open_store(&s, 100, 1, 4);
    uint64_t k[] = {1, 2};
    CHECK(!publish(&s, "good", k, 2, false));
    s.fail_at = step;
    CHECK(publish(&s, "never published", k, 1, false) != NULL);
    CHECK(s.current.generation == 1); // in memory too
    pso_gen_close(&s);
    struct pso_gen_store t = {0};
    open_store(&t, 200, 2, 4);
    if (t.current.generation != 1 || t.current.shard_count != 1 || t.current.shards[0].key_count != 2)
      printf("step %d: generation %llu\n%s", step, (unsigned long long)t.current.generation, g_report);
    CHECK(t.current.generation == 1 && t.current.shard_count == 1 && t.current.shards[0].key_count == 2);
    CHECK(count_files(".tmp") == 0); // the writer collected any leftover
    CHECK(count_files(".shard") == 1);
    CHECK(t.next_generation >= 2);
    // and the store goes on publishing
    CHECK(!publish(&t, "after the fault", k, 2, false));
    CHECK(t.current.generation == t.next_generation - 1 && t.current.shard_count == 2);
    pso_gen_close(&t);
  }
}

static void
test_callback_failure(void) {
  reset_dir();
  struct pso_gen_store s = {0};
  open_store(&s, 100, 1, 4);
  char why[256];
  struct fake f = {"x", "Failed to generate machO: missing 'vertex' stage in pipeline no. '4'"};
  const char *failed = pso_gen_publish(&s, fake_write, &f, NULL, 0, false, NULL, 0, NULL, why, sizeof(why));
  CHECK(failed && strstr(failed, "pipeline no. '4'")); // the adapter's text, for its policy
  CHECK(s.current.generation == 0 && count_files(".tmp") == 0);
  pso_gen_close(&s);
}

static void
corrupt(const char *name, long offset) {
  char path[1200];
  snprintf(path, sizeof(path), "%s/%s", g_dir, name);
  int fd = open(path, O_RDWR);
  CHECK(fd >= 0);
  unsigned char c = 0;
  pread(fd, &c, 1, offset);
  c ^= 0x40;
  pwrite(fd, &c, 1, offset);
  close(fd);
}

static void
test_corruption(void) {
  reset_dir();
  struct pso_gen_store s = {0};
  open_store(&s, 100, 1, 4);
  uint64_t k[] = {1};
  CHECK(!publish(&s, "first shard", k, 1, false));
  CHECK(!publish(&s, "second shard", k, 1, false));
  char shard2[100];
  {
    char path[1200];
    pso_gen_shard_path(&s, &s.current.shards[0], path, sizeof(path));
    snprintf(shard2, sizeof(shard2), "%s", strrchr(path, '/') + 1);
  }
  pso_gen_close(&s);

  // a corrupt shard is dropped; the generation and its other shard stay
  corrupt(shard2, 2);
  struct pso_gen_store t = {0};
  open_store(&t, 200, 2, 4);
  CHECK(t.current.generation == 2 && t.current.shard_count == 1);
  CHECK(strstr(g_report, "dropped shard") && strstr(g_report, "does not match"));
  pso_gen_close(&t);

  // a corrupt current manifest: the previous generation (the last good one)
  reset_dir();
  open_store(&s, 100, 1, 4);
  CHECK(!publish(&s, "a", k, 1, false));
  CHECK(!publish(&s, "b", k, 1, false));
  pso_gen_close(&s);
  corrupt("g0000000000000002.manifest", 20);
  struct pso_gen_store u = {0};
  open_store(&u, 200, 2, 4);
  CHECK(u.current.generation == 1 && u.current.shard_count == 1);
  CHECK(strstr(g_report, "generation 2 failed verification") && strstr(g_report, "last good"));
  CHECK(u.next_generation == 3); // never reuses a generation number on disk
  CHECK(!publish(&u, "c", k, 1, false) && u.current.generation == 3 && u.current.prev == 1);
  pso_gen_close(&u);

  // a garbled index: the newest manifest that verifies
  char path[1200];
  snprintf(path, sizeof(path), "%s/CURRENT", g_dir);
  FILE *fp = fopen(path, "w");
  fputs("garbage", fp);
  fclose(fp);
  struct pso_gen_store v = {0};
  open_store(&v, 300, 3, 4);
  CHECK(v.current.generation == 3);
  CHECK(strstr(g_report, "no valid index"));
  pso_gen_close(&v);

  // no index and no manifest: empty
  reset_dir();
  struct pso_gen_store w = {0};
  open_store(&w, 300, 3, 4);
  CHECK(w.current.generation == 0 && w.current.shard_count == 0 && w.next_generation == 1);
  pso_gen_close(&w);
}

static void
test_lock(void) {
  reset_dir();
  struct pso_gen_store a = {0}, b = {0};
  open_store(&a, 100, 111, 4);
  uint64_t k[] = {1};
  CHECK(!publish(&a, "a", k, 1, false));
  open_store(&b, 200, 222, 4);
  CHECK(a.role == PSO_GEN_WRITER && b.role == PSO_GEN_READER);
  CHECK(strstr(g_report, "pid 111") && strstr(g_report, "lookup only"));
  CHECK(b.current.generation == 1); // a reader loads
  CHECK(publish(&b, "b", k, 1, false) != NULL); // and never writes
  CHECK(count_files(".shard") == 1);
  pso_gen_close(&a);
  pso_gen_close(&b);
  struct pso_gen_store c = {0};
  open_store(&c, 300, 333, 4);
  CHECK(c.role == PSO_GEN_WRITER);
  pso_gen_close(&c);
}

static void
test_sessions_kept(void) {
  reset_dir();
  uint64_t k[] = {1};
  for (uint64_t session = 1; session <= 4; session++) {
    struct pso_gen_store s = {0};
    open_store(&s, 1000 + session, session, 2);
    char content[32];
    snprintf(content, sizeof(content), "session %llu a", (unsigned long long)session);
    CHECK(!publish(&s, content, k, 1, false));
    snprintf(content, sizeof(content), "session %llu b", (unsigned long long)session);
    CHECK(!publish(&s, content, k, 1, false));
    // this session's two shards and the newest other session's two
    CHECK(s.current.shard_count == (session == 1 ? 2 : 4));
    CHECK(s.current.shards[0].started == 1000 + session);
    pso_gen_close(&s);
  }
  struct pso_gen_store s = {0};
  open_store(&s, 2000, 9, 2);
  CHECK(s.current.shard_count == 4 && s.current.shards[0].started == 1004 && s.current.shards[3].started == 1003);
  pso_gen_close(&s);
  // a smaller keep at load drops the older sessions' shards
  open_store(&s, 2000, 9, 1);
  CHECK(s.current.shard_count == 2 && s.current.shards[1].started == 1004);
  CHECK(strstr(g_report, "older than the newest sessions kept"));
  pso_gen_close(&s);
}

static void
test_excluded_and_codec(void) {
  reset_dir();
  struct pso_gen_store s = {0};
  open_store(&s, 100, 1, 4);
  char why[256];
  struct fake f = {"x", NULL};
  uint64_t ex1[] = {5}, ex2[] = {6, 5};
  CHECK(!pso_gen_publish(&s, fake_write, &f, NULL, 0, false, ex1, 1, NULL, why, sizeof(why)));
  CHECK(!pso_gen_publish(&s, fake_write, &f, NULL, 0, false, ex2, 2, NULL, why, sizeof(why)));
  CHECK(s.current.excluded_count == 2 && s.current.excluded[0] == 5 && s.current.excluded[1] == 6);
  pso_gen_close(&s);
  struct pso_gen_store t = {0};
  open_store(&t, 200, 2, 4);
  CHECK(t.current.excluded_count == 2);
  CHECK(!pso_gen_publish(&t, fake_write, &f, NULL, 0, false, NULL, 0, NULL, why, sizeof(why)));
  CHECK(t.current.excluded_count == 2); // carried
  uint8_t *data;
  size_t size;
  CHECK(pso_gen_encode(&t.current, &data, &size));
  struct pso_gen_manifest m;
  CHECK(pso_gen_decode(data, size, &m) && m.generation == t.current.generation && m.shard_count == 3);
  pso_gen_manifest_free(&m);
  CHECK(!pso_gen_decode(data, size - 1, &m)); // truncated
  data[size / 2] ^= 1;
  CHECK(!pso_gen_decode(data, size, &m)); // checksum
  free(data);
  pso_gen_close(&t);
}

// #84 review: an exclusion-only generation (no shard) carries every shard and adds the shapes; a
// failure at any of its steps (4-7; 1-3 write no shard) leaves the last good generation
static void
test_exclusion_only(void) {
  for (int step = 0; step <= 7; step++) {
    reset_dir();
    struct pso_gen_store s = {0};
    open_store(&s, 100, 1, 4);
    uint64_t k[] = {1, 2};
    CHECK(!publish(&s, "the only shard", k, 2, false));
    pso_gen_close(&s);
    struct pso_gen_store t = {0};
    open_store(&t, 200, 2, 4);
    t.fail_at = step;
    char why[256];
    uint64_t ex[] = {9}, bytes = 77;
    const char *failed = pso_gen_publish(&t, NULL, NULL, NULL, 0, true, ex, 1, &bytes, why, sizeof(why));
    bool should_fail = step >= 4;
    CHECK(!failed == !should_fail);
    if (!failed)
      CHECK(bytes == 0 && t.current.generation == 2 && t.current.shard_count == 1 && t.current.excluded_count == 1 &&
            t.current.excluded[0] == 9 && pso_gen_session_shards(&t) == 0);
    pso_gen_close(&t);
    struct pso_gen_store u = {0};
    open_store(&u, 300, 3, 4);
    CHECK(u.current.generation == (should_fail ? 1u : 2u));
    CHECK(u.current.shard_count == 1 && u.current.shards[0].key_count == 2);
    CHECK(u.current.excluded_count == (should_fail ? 0u : 1u));
    CHECK(count_files(".shard") == 1 && count_files(".tmp") == 0);
    pso_gen_close(&u);
  }
}

// #89: an exclusion-only generation adds no shard, so it keeps every session the current one holds,
// at the retention boundary too (keep_sessions earlier sessions; keep 1 with its sole shard)
static void
test_exclusion_only_retention(void) {
  for (uint32_t keep = 1; keep <= 4; keep += 3) {
    reset_dir();
    uint64_t k[] = {1};
    for (uint64_t session = 1; session <= keep; session++) {
      struct pso_gen_store s = {0};
      open_store(&s, 1000 + session, session, keep);
      char content[32];
      snprintf(content, sizeof(content), "session %llu", (unsigned long long)session);
      CHECK(!publish(&s, content, k, 1, false));
      pso_gen_close(&s);
    }
    struct pso_gen_store t = {0};
    open_store(&t, 2000, 99, keep);
    CHECK(t.current.shard_count == keep);
    char why[256];
    uint64_t ex[] = {9};
    CHECK(!pso_gen_publish(&t, NULL, NULL, NULL, 0, false, ex, 1, NULL, why, sizeof(why)));
    CHECK(t.current.shard_count == keep && t.current.excluded_count == 1);
    CHECK(t.current.shards[keep - 1].started == 1001); // the oldest session stays
    pso_gen_close(&t);
    struct pso_gen_store u = {0};
    open_store(&u, 3000, 100, keep);
    CHECK(u.current.shard_count == keep && u.current.excluded_count == 1);
    CHECK(count_files(".shard") == keep);
    pso_gen_close(&u);
  }
}

int
main(int argc, char **argv) {
  snprintf(g_dir, sizeof(g_dir), "%s", argc > 1 ? argv[1] : "/tmp/pso_archive_publish_test");
  test_round_trip();
  test_fault_every_step();
  test_callback_failure();
  test_corruption();
  test_lock();
  test_sessions_kept();
  test_excluded_and_codec();
  test_exclusion_only();
  test_exclusion_only_retention();
  if (failures) {
    printf("%d failures\n", failures);
    return 1;
  }
  printf("pso_archive_publish_test: ok\n");
  return 0;
}
