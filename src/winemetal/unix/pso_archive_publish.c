/* gw2 (#75): the PSO archive's generation publisher. See pso_archive_publish.h. */

#include "pso_archive_publish.h"
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MANIFEST_MAGIC "DXMTGEN1"
#define INDEX_MAGIC "DXMTCUR1"
#define MAX_SHARDS 1024
#define MAX_EXCLUDED 65536

void
pso_report_line(struct pso_report *r, const char *fmt, ...) {
  if (!r || !r->buf || r->len + 2 >= r->size)
    return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(r->buf + r->len, r->size - r->len - 1, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  r->len += (size_t)n < r->size - r->len - 1 ? (size_t)n : r->size - r->len - 2;
  r->buf[r->len++] = '\n';
  r->buf[r->len] = '\0';
}

static void
hex(const uint8_t *sha, char out[65]) {
  for (unsigned i = 0; i < 32; i++)
    snprintf(out + i * 2, 3, "%02x", sha[i]);
}

static bool
unhex(const char *s, uint8_t sha[32]) {
  for (unsigned i = 0; i < 32; i++) {
    unsigned v;
    if (sscanf(s + i * 2, "%2x", &v) != 1)
      return false;
    sha[i] = (uint8_t)v;
  }
  return true;
}

static bool
sha_file(const char *path, uint64_t *size, uint8_t sha[32]) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  CC_SHA256_CTX ctx;
  CC_SHA256_Init(&ctx);
  static const size_t kBuf = 1 << 20;
  uint8_t *buf = malloc(kBuf);
  uint64_t total = 0;
  bool ok = buf != NULL;
  while (ok) {
    ssize_t n = read(fd, buf, kBuf);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      ok = false;
    if (n <= 0)
      break;
    CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    total += (uint64_t)n;
  }
  free(buf);
  close(fd);
  CC_SHA256_Final(sha, &ctx);
  *size = total;
  return ok;
}

static bool
write_all(int fd, const void *p, size_t n) {
  const uint8_t *b = p;
  while (n) {
    ssize_t w = write(fd, b, n);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return false;
    b += w;
    n -= (size_t)w;
  }
  return true;
}

/* writes `data` to `path` and syncs it; false (file removed) on failure */
static bool
write_synced(const char *path, const void *data, size_t n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return false;
  bool ok = write_all(fd, data, n) && !fsync(fd);
  ok = !close(fd) && ok;
  if (!ok)
    unlink(path);
  return ok;
}

static bool
sync_path(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  bool ok = !fsync(fd);
  close(fd);
  return ok;
}

static bool
read_file(const char *path, uint8_t **data, size_t *size) {
  *data = NULL;
  *size = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  struct stat st;
  if (fstat(fd, &st) || st.st_size < 0 || st.st_size > (1 << 30)) {
    close(fd);
    return false;
  }
  uint8_t *buf = malloc(st.st_size ? (size_t)st.st_size : 1);
  size_t got = 0;
  while (buf && got < (size_t)st.st_size) {
    ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    got += (size_t)n;
  }
  close(fd);
  if (!buf || got != (size_t)st.st_size) {
    free(buf);
    return false;
  }
  *data = buf;
  *size = got;
  return true;
}

/* --- manifests */

struct buf {
  uint8_t *p;
  size_t len, cap;
  bool failed;
};

static void
buf_put(struct buf *b, const void *p, size_t n) {
  if (b->failed)
    return;
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + n)
      cap *= 2;
    uint8_t *grown = realloc(b->p, cap);
    if (!grown) {
      b->failed = true;
      return;
    }
    b->p = grown;
    b->cap = cap;
  }
  memcpy(b->p + b->len, p, n);
  b->len += n;
}

static void
buf_u64(struct buf *b, uint64_t v) {
  buf_put(b, &v, sizeof(v));
}

bool
pso_gen_encode(const struct pso_gen_manifest *m, uint8_t **out, size_t *out_size) {
  struct buf b = {0};
  buf_put(&b, MANIFEST_MAGIC, 8);
  buf_u64(&b, m->generation);
  buf_u64(&b, m->prev);
  buf_u64(&b, m->shard_count);
  buf_u64(&b, m->excluded_count);
  for (uint64_t i = 0; i < m->shard_count; i++) {
    const struct pso_gen_shard *sh = &m->shards[i];
    buf_u64(&b, sh->started);
    buf_u64(&b, sh->pid);
    buf_u64(&b, sh->size);
    buf_put(&b, sh->sha256, 32);
    buf_u64(&b, sh->key_count);
    if (sh->key_count)
      buf_put(&b, sh->keys, sh->key_count * sizeof(uint64_t));
  }
  if (m->excluded_count)
    buf_put(&b, m->excluded, m->excluded_count * sizeof(uint64_t));
  uint8_t sum[32];
  if (!b.failed)
    CC_SHA256(b.p, (CC_LONG)b.len, sum);
  buf_put(&b, sum, 32);
  if (b.failed) {
    free(b.p);
    return false;
  }
  *out = b.p;
  *out_size = b.len;
  return true;
}

struct reader {
  const uint8_t *p;
  size_t left;
};

static bool
rd(struct reader *r, void *out, size_t n) {
  if (r->left < n)
    return false;
  memcpy(out, r->p, n);
  r->p += n;
  r->left -= n;
  return true;
}

static bool
rd_u64s(struct reader *r, uint64_t count, uint64_t **out) {
  *out = NULL;
  if (!count)
    return true;
  if (count > r->left / sizeof(uint64_t))
    return false;
  *out = malloc(count * sizeof(uint64_t));
  return *out && rd(r, *out, count * sizeof(uint64_t));
}

void
pso_gen_manifest_free(struct pso_gen_manifest *m) {
  for (uint64_t i = 0; i < m->shard_count; i++)
    free(m->shards[i].keys);
  free(m->shards);
  free(m->excluded);
  memset(m, 0, sizeof(*m));
}

bool
pso_gen_decode(const uint8_t *data, size_t size, struct pso_gen_manifest *m) {
  memset(m, 0, sizeof(*m));
  if (size < 8 + 4 * 8 + 32 || memcmp(data, MANIFEST_MAGIC, 8))
    return false;
  uint8_t sum[32];
  CC_SHA256(data, (CC_LONG)(size - 32), sum);
  if (memcmp(sum, data + size - 32, 32))
    return false;
  struct reader r = {data + 8, size - 8 - 32};
  if (!rd(&r, &m->generation, 8) || !rd(&r, &m->prev, 8) || !rd(&r, &m->shard_count, 8) ||
      !rd(&r, &m->excluded_count, 8) || m->shard_count > MAX_SHARDS || m->excluded_count > MAX_EXCLUDED) {
    m->shard_count = 0;
    pso_gen_manifest_free(m);
    return false;
  }
  uint64_t shards = m->shard_count;
  m->shard_count = 0;
  m->shards = shards ? calloc(shards, sizeof(*m->shards)) : NULL;
  if (shards && !m->shards) {
    pso_gen_manifest_free(m);
    return false;
  }
  for (uint64_t i = 0; i < shards; i++) {
    struct pso_gen_shard *sh = &m->shards[i];
    m->shard_count = i + 1; /* so a failure frees what was read */
    if (!rd(&r, &sh->started, 8) || !rd(&r, &sh->pid, 8) || !rd(&r, &sh->size, 8) || !rd(&r, sh->sha256, 32) ||
        !rd(&r, &sh->key_count, 8) || !rd_u64s(&r, sh->key_count, &sh->keys)) {
      pso_gen_manifest_free(m);
      return false;
    }
    sh->usable = true;
  }
  uint64_t excluded = m->excluded_count;
  if (!rd_u64s(&r, excluded, &m->excluded) || r.left) {
    pso_gen_manifest_free(m);
    return false;
  }
  return true;
}

/* --- the store */

static void
manifest_path(const struct pso_gen_store *s, uint64_t gen, char *buf, size_t size) {
  snprintf(buf, size, "%s/g%016llx.manifest", s->dir, (unsigned long long)gen);
}

void
pso_gen_shard_path(const struct pso_gen_store *s, const struct pso_gen_shard *shard, char *buf, size_t size) {
  char h[65];
  hex(shard->sha256, h);
  snprintf(buf, size, "%s/%s.shard", s->dir, h);
}

static void
index_path(const struct pso_gen_store *s, char *buf, size_t size) {
  snprintf(buf, size, "%s/CURRENT", s->dir);
}

/* CURRENT's generation and manifest SHA-256; false if missing or malformed */
static bool
read_index(const struct pso_gen_store *s, uint64_t *gen, uint8_t sha[32]) {
  char path[PATH_MAX + 16];
  index_path(s, path, sizeof(path));
  uint8_t *data;
  size_t size;
  if (!read_file(path, &data, &size))
    return false;
  char text[128] = "";
  memcpy(text, data, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
  free(data);
  char shahex[65] = "";
  unsigned long long g = 0;
  if (sscanf(text, INDEX_MAGIC " %16llx %64s", &g, shahex) != 2 || strlen(shahex) != 64 || !unhex(shahex, sha) || !g)
    return false;
  *gen = g;
  return true;
}

/* loads manifest `gen`; with `want`, its file must hash to it; NULL or why not */
static const char *
load_manifest(const struct pso_gen_store *s, uint64_t gen, const uint8_t *want, struct pso_gen_manifest *m) {
  char path[PATH_MAX + 32];
  manifest_path(s, gen, path, sizeof(path));
  uint8_t *data;
  size_t size;
  if (!read_file(path, &data, &size))
    return "manifest missing or unreadable";
  const char *why = NULL;
  uint8_t sha[32];
  CC_SHA256(data, (CC_LONG)size, sha);
  if (want && memcmp(sha, want, 32))
    why = "manifest does not match the index";
  else if (!pso_gen_decode(data, size, m))
    why = "manifest malformed or its checksum does not match";
  else if (m->generation != gen) {
    why = "manifest names another generation";
    pso_gen_manifest_free(m);
  }
  free(data);
  return why;
}

static int
shard_newer_first(const void *a, const void *b) {
  const struct pso_gen_shard *x = a, *y = b;
  if (x->started != y->started)
    return x->started > y->started ? -1 : 1;
  if (x->pid != y->pid)
    return x->pid > y->pid ? -1 : 1;
  return 0;
}

/* drops shards whose file doesn't match, sorts newest session first, keeps keep_sessions sessions */
static void
verify_shards(struct pso_gen_store *s, struct pso_gen_manifest *m, struct pso_report *r) {
  uint64_t kept = 0, sessions = 0;
  qsort(m->shards, m->shard_count, sizeof(*m->shards), shard_newer_first);
  for (uint64_t i = 0; i < m->shard_count; i++) {
    struct pso_gen_shard *sh = &m->shards[i];
    if (!i || sh->started != m->shards[i - 1].started || sh->pid != m->shards[i - 1].pid)
      sessions++;
    char path[PATH_MAX + 80], h[65];
    pso_gen_shard_path(s, sh, path, sizeof(path));
    hex(sh->sha256, h);
    uint64_t size = 0;
    uint8_t sha[32];
    const char *why = NULL;
    if (sessions > s->keep_sessions)
      why = "older than the newest sessions kept";
    else if (!sha_file(path, &size, sha))
      why = "missing or unreadable";
    else if (size != sh->size || memcmp(sha, sh->sha256, 32))
      why = "size or SHA-256 does not match";
    if (why) {
      pso_report_line(r, "PSO archive: dropped shard %.12s (session %llu-%llu) of generation %llu: %s", h,
                      (unsigned long long)sh->started, (unsigned long long)sh->pid,
                      (unsigned long long)m->generation, why);
      free(sh->keys);
      continue;
    }
    m->shards[kept++] = *sh;
  }
  m->shard_count = kept;
}

static bool
ends_with(const char *s, const char *suffix) {
  size_t n = strlen(s), k = strlen(suffix);
  return n >= k && !strcmp(s + n - k, suffix);
}

static bool
manifest_has_shard(const struct pso_gen_manifest *m, const char *hexname) {
  for (uint64_t i = 0; m && i < m->shard_count; i++) {
    char h[65];
    hex(m->shards[i].sha256, h);
    if (!strncmp(h, hexname, 64))
      return true;
  }
  return false;
}

/* Writer only: removes leftovers (.tmp), manifests other than `keep_a`'s and `keep_b`'s and shards
   neither references; the number of files removed. */
static unsigned
collect(const struct pso_gen_store *s, const struct pso_gen_manifest *keep_a, const struct pso_gen_manifest *keep_b) {
  DIR *d = opendir(s->dir);
  if (!d)
    return 0;
  unsigned removed = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    const char *n = e->d_name;
    bool drop = false;
    if (ends_with(n, ".tmp")) {
      drop = true;
    } else if (n[0] == 'g' && ends_with(n, ".manifest")) {
      unsigned long long g = strtoull(n + 1, NULL, 16);
      drop = !(keep_a && keep_a->generation && g == keep_a->generation) &&
             !(keep_b && keep_b->generation && g == keep_b->generation);
    } else if (ends_with(n, ".shard") && strlen(n) == 64 + 6) {
      drop = !manifest_has_shard(keep_a, n) && !manifest_has_shard(keep_b, n);
    }
    if (drop) {
      char path[PATH_MAX + 260];
      snprintf(path, sizeof(path), "%s/%s", s->dir, n);
      removed += !unlink(path);
    }
  }
  closedir(d);
  return removed;
}

/* the highest generation of any manifest file in the store, verified or not */
static uint64_t
max_manifest_on_disk(const struct pso_gen_store *s) {
  DIR *d = opendir(s->dir);
  if (!d)
    return 0;
  uint64_t max = 0;
  struct dirent *e;
  while ((e = readdir(d)))
    if (e->d_name[0] == 'g' && ends_with(e->d_name, ".manifest")) {
      uint64_t g = strtoull(e->d_name + 1, NULL, 16);
      if (g > max)
        max = g;
    }
  closedir(d);
  return max;
}

/* the newest manifest below `below` (0: any) that loads, into `m`; its generation or 0 */
static uint64_t
load_newest_good(const struct pso_gen_store *s, uint64_t below, struct pso_gen_manifest *m) {
  for (uint64_t g = max_manifest_on_disk(s); g; ) {
    /* walk down through the manifests present, newest first */
    if (!below || g < below) {
      if (!load_manifest(s, g, NULL, m))
        return g;
    }
    uint64_t next = 0;
    DIR *d = opendir(s->dir);
    if (!d)
      return 0;
    struct dirent *e;
    while ((e = readdir(d)))
      if (e->d_name[0] == 'g' && ends_with(e->d_name, ".manifest")) {
        uint64_t x = strtoull(e->d_name + 1, NULL, 16);
        if (x < g && x > next)
          next = x;
      }
    closedir(d);
    g = next;
  }
  return 0;
}

bool
pso_gen_open(struct pso_gen_store *s, const char *dir, uint32_t keep_sessions, uint64_t started, uint64_t pid,
             struct pso_report *r) {
  int fail_at = s->fail_at;
  memset(s, 0, sizeof(*s));
  s->fail_at = fail_at;
  s->lock_fd = -1;
  snprintf(s->dir, sizeof(s->dir), "%s", dir);
  s->keep_sessions = keep_sessions ? keep_sessions : 1;
  s->started = started;
  s->pid = pid;
  if (mkdir(dir, 0755) && errno != EEXIST) {
    pso_report_line(r, "PSO archive: off: cannot create %s", dir);
    return false;
  }
  char path[PATH_MAX + 16];
  snprintf(path, sizeof(path), "%s/LOCK", dir);
  s->lock_fd = open(path, O_RDWR | O_CREAT, 0644);
  if (s->lock_fd < 0) {
    pso_report_line(r, "PSO archive: off: cannot open %s", path);
    return false;
  }
  if (!flock(s->lock_fd, LOCK_EX | LOCK_NB)) {
    s->role = PSO_GEN_WRITER;
    char text[32];
    int n = snprintf(text, sizeof(text), "%llu\n", (unsigned long long)pid);
    if (ftruncate(s->lock_fd, 0) || pwrite(s->lock_fd, text, (size_t)n, 0) != n)
      pso_report_line(r, "PSO archive: cannot write this process's pid into %s (the lock holds)", path);
  } else {
    s->role = PSO_GEN_READER;
    char text[32] = "";
    ssize_t n = pread(s->lock_fd, text, sizeof(text) - 1, 0);
    text[n > 0 ? n : 0] = '\0';
    pso_report_line(r, "PSO archive: another process (pid %llu) writes %s: lookup only, this session captures nothing",
                    strtoull(text, NULL, 10), dir);
  }

  uint64_t want_gen = 0;
  uint8_t want_sha[32];
  bool indexed = read_index(s, &want_gen, want_sha);
  const char *why = indexed ? load_manifest(s, want_gen, want_sha, &s->current) : NULL;
  if (!indexed || why) {
    uint64_t got = load_newest_good(s, indexed ? want_gen : 0, &s->current);
    if (indexed)
      pso_report_line(r, "PSO archive: generation %llu failed verification: %s; %s", (unsigned long long)want_gen, why,
                      got ? "using the last good generation" : "no earlier generation verifies, starting empty");
    else if (got)
      pso_report_line(r, "PSO archive: no valid index (CURRENT); using generation %llu, the newest that verifies",
                      (unsigned long long)got);
  }
  if (s->current.generation)
    verify_shards(s, &s->current, r);
  uint64_t on_disk = max_manifest_on_disk(s);
  s->next_generation = (on_disk > s->current.generation ? on_disk : s->current.generation) + 1;

  if (s->role == PSO_GEN_WRITER) {
    struct pso_gen_manifest prev = {0};
    if (s->current.prev && load_manifest(s, s->current.prev, NULL, &prev))
      memset(&prev, 0, sizeof(prev));
    unsigned removed = collect(s, &s->current, &prev);
    pso_gen_manifest_free(&prev);
    if (removed)
      pso_report_line(r, "PSO archive: removed %u files no generation since the previous one uses (or leftovers)",
                      removed);
  }
  return true;
}

void
pso_gen_close(struct pso_gen_store *s) {
  pso_gen_manifest_free(&s->current);
  if (s->lock_fd >= 0)
    close(s->lock_fd); /* releases the flock */
  s->lock_fd = -1;
}

static uint64_t *
dup_keys(const uint64_t *keys, uint64_t n, bool *ok) {
  if (!n)
    return NULL;
  uint64_t *copy = malloc(n * sizeof(uint64_t));
  if (!copy) {
    *ok = false;
    return NULL;
  }
  memcpy(copy, keys, n * sizeof(uint64_t));
  return copy;
}

static bool
has_key(const uint64_t *keys, uint64_t n, uint64_t key) {
  for (uint64_t i = 0; i < n; i++)
    if (keys[i] == key)
      return true;
  return false;
}

static const char *
fail(char *why, size_t why_size, const char *step) {
  snprintf(why, why_size, "%s", step);
  return why;
}

uint64_t
pso_gen_session_shards(const struct pso_gen_store *s) {
  uint64_t n = 0;
  for (uint64_t i = 0; i < s->current.shard_count; i++)
    n += s->current.shards[i].started == s->started && s->current.shards[i].pid == s->pid;
  return n;
}

const char *
pso_gen_publish(struct pso_gen_store *s, pso_gen_write_fn write, void *ctx, const uint64_t *keys, uint64_t key_count,
                bool replace_session, const uint64_t *excluded, uint64_t excluded_count, uint64_t *bytes, char *why,
                size_t why_size) {
  if (s->role != PSO_GEN_WRITER)
    return fail(why, why_size, "not the store's writer");
  char tmp[PATH_MAX + 64], final[PATH_MAX + 80];
  snprintf(tmp, sizeof(tmp), "%s/shard-%llu.tmp", s->dir, (unsigned long long)s->pid);

  /* 1-3: the shard, complete and synced, then in place under its content's name (none for an
     exclusion-only generation) */
  struct pso_gen_shard shard = {.started = s->started, .pid = s->pid, .usable = true};
  if (write) {
    if (s->fail_at == 1)
      return fail(why, why_size, "cannot write the shard (test)");
    const char *failed = write(ctx, tmp, why, why_size);
    if (failed) {
      unlink(tmp);
      return failed;
    }
    if (s->fail_at == 2 || !sync_path(tmp) || !sha_file(tmp, &shard.size, shard.sha256)) {
      unlink(tmp);
      return fail(why, why_size, "cannot sync or read back the shard");
    }
    pso_gen_shard_path(s, &shard, final, sizeof(final));
    if (s->fail_at == 3 || rename(tmp, final)) {
      unlink(tmp);
      return fail(why, why_size, "cannot rename the shard into place");
    }
  } else {
    replace_session = false; /* nothing replaces this session's shards */
  }

  /* the new generation: this session's new shard and (unless it replaces them) its earlier ones,
     then every usable shard of the newest keep_sessions - 1 other sessions */
  struct pso_gen_manifest m = {.generation = s->next_generation, .prev = s->current.generation};
  bool ok = true;
  m.shards = calloc(s->current.shard_count + 1, sizeof(*m.shards));
  ok = m.shards != NULL;
  if (ok && write) {
    shard.key_count = key_count;
    shard.keys = dup_keys(keys, key_count, &ok);
    m.shards[m.shard_count++] = shard;
  }
  uint64_t others = 0;
  for (uint64_t i = 0; ok && i < s->current.shard_count; i++) {
    const struct pso_gen_shard *old = &s->current.shards[i];
    bool mine = old->started == s->started && old->pid == s->pid;
    if (!mine && (!i || old->started != s->current.shards[i - 1].started || old->pid != s->current.shards[i - 1].pid))
      others++; /* shards are grouped by session, newest first */
    if (!old->usable || (mine && replace_session) || (!mine && others > s->keep_sessions - 1))
      continue;
    struct pso_gen_shard *copy = &m.shards[m.shard_count++];
    *copy = *old;
    copy->keys = dup_keys(old->keys, old->key_count, &ok);
  }
  qsort(m.shards, m.shard_count, sizeof(*m.shards), shard_newer_first);
  /* excluded shapes: the current generation's and this session's, each once */
  uint64_t ex_cap = s->current.excluded_count + excluded_count;
  if (ex_cap > MAX_EXCLUDED)
    ex_cap = MAX_EXCLUDED;
  m.excluded = ok && ex_cap ? malloc(ex_cap * sizeof(uint64_t)) : NULL;
  ok = ok && (!ex_cap || m.excluded);
  for (uint64_t i = 0; ok && i < s->current.excluded_count + excluded_count && m.excluded_count < ex_cap; i++) {
    uint64_t k = i < s->current.excluded_count ? s->current.excluded[i] : excluded[i - s->current.excluded_count];
    if (!has_key(m.excluded, m.excluded_count, k))
      m.excluded[m.excluded_count++] = k;
  }
  uint8_t *data = NULL;
  size_t size = 0;
  if (!ok || !pso_gen_encode(&m, &data, &size)) {
    pso_gen_manifest_free(&m);
    return fail(why, why_size, "out of memory for the manifest");
  }

  /* 4-5: the manifest, synced, then in place */
  char mtmp[PATH_MAX + 64], mpath[PATH_MAX + 32];
  snprintf(mtmp, sizeof(mtmp), "%s/manifest-%llu.tmp", s->dir, (unsigned long long)s->pid);
  manifest_path(s, m.generation, mpath, sizeof(mpath));
  uint8_t msha[32];
  CC_SHA256(data, (CC_LONG)size, msha);
  bool wrote = s->fail_at != 4 && write_synced(mtmp, data, size);
  free(data);
  if (!wrote || s->fail_at == 5 || rename(mtmp, mpath)) {
    unlink(mtmp);
    pso_gen_manifest_free(&m);
    return fail(why, why_size, wrote ? "cannot rename the manifest into place" : "cannot write the manifest");
  }

  /* 6-7: the index, the one file replaced: the generation is published when this rename lands */
  char itmp[PATH_MAX + 64], ipath[PATH_MAX + 16], text[128], h[65];
  snprintf(itmp, sizeof(itmp), "%s/CURRENT-%llu.tmp", s->dir, (unsigned long long)s->pid);
  index_path(s, ipath, sizeof(ipath));
  hex(msha, h);
  int n = snprintf(text, sizeof(text), INDEX_MAGIC " %016llx %s\n", (unsigned long long)m.generation, h);
  wrote = s->fail_at != 6 && write_synced(itmp, text, (size_t)n);
  if (!wrote || s->fail_at == 7 || rename(itmp, ipath)) {
    unlink(itmp);
    pso_gen_manifest_free(&m);
    return fail(why, why_size, wrote ? "cannot replace the index" : "cannot write the index");
  }
  sync_path(s->dir);

  /* published: the old generation becomes the previous one, everything older goes */
  struct pso_gen_manifest old = s->current;
  s->current = m;
  s->next_generation++;
  s->published++;
  if (write)
    memcpy(s->last_sha256, shard.sha256, 32);
  collect(s, &s->current, &old);
  pso_gen_manifest_free(&old);
  if (bytes)
    *bytes = shard.size;
  return NULL;
}
