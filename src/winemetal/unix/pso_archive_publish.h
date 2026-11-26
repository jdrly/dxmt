#pragma once

/* gw2 (#75): the PSO archive's generation publisher (DXMT_PSO_GENERATIONS=1), pure (plain C, POSIX
 * and CommonCrypto; no Metal). The perf council's durable archives
 * (council/2026-11-04-perf-shaders/final.md, "Durable binary archives"):
 *
 * - A store is one directory (pso_archive.c: <os-build>-<GPU dir>/gen1). It holds immutable shards,
 *   <sha256>.shard, named by their content and never rewritten; one manifest per generation,
 *   g<generation, 16 hex>.manifest; and the index, CURRENT ("DXMTCUR1 <generation> <manifest
 *   sha256>"), the only file ever replaced.
 * - A manifest lists, newest session first, each shard's session (start time, pid), size, SHA-256
 *   and persisted keys (the pipelines in it), then the excluded shapes (keys Metal's packer named),
 *   then a SHA-256 over all of that.
 * - Publishing writes the shard through the adapter's callback, syncs and renames it into place,
 *   writes and syncs the manifest, and only then replaces CURRENT (write, sync, rename, sync the
 *   directory). A failure at any step leaves CURRENT, and so the last good generation, as it was.
 *   A session publishes one shard per flush: the pipelines since its last one (a delta), or, to merge,
 *   all of them, replacing its earlier shards. A new generation carries this session's shards and
 *   every usable shard of the newest keep_sessions - 1 other sessions.
 * - Loading takes CURRENT's generation if its manifest verifies, else the newest older manifest that
 *   does (the last good generation); a shard whose size or SHA-256 doesn't match is dropped.
 * - One writer per store: an exclusive flock on LOCK, held while the store is open. Another
 *   process gets the reader role: it loads and never writes or deletes. Only the writer collects
 *   files: everything but the current and the previous generation's manifests and shards.
 *
 * The shard's format is the adapter's (an MTLBinaryArchive's serialization for pso_archive.c; an
 * MTL4Archive's for #34): the publisher only names, checks and publishes files.
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Report lines ('\n'-terminated, UTF-8) into a caller's buffer; silently truncated when full. */
struct pso_report {
  char *buf;
  size_t size;
  size_t len;
};
void pso_report_line(struct pso_report *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

struct pso_gen_shard {
  uint64_t started; /* the session's start (unix time) */
  uint64_t pid;
  uint64_t size;
  uint8_t sha256[32];
  uint64_t key_count;
  uint64_t *keys;
  bool usable; /* verified; the adapter clears it when it can't load the shard */
};

struct pso_gen_manifest {
  uint64_t generation; /* 0: none */
  uint64_t prev;       /* the generation it replaced; 0: none */
  uint64_t shard_count;
  struct pso_gen_shard *shards; /* newest session first */
  uint64_t excluded_count;
  uint64_t *excluded;
};

enum pso_gen_role {
  PSO_GEN_WRITER = 1,
  PSO_GEN_READER = 2, /* another process holds the lock */
};

struct pso_gen_store {
  char dir[PATH_MAX];
  int lock_fd;
  enum pso_gen_role role;
  uint32_t keep_sessions;
  uint64_t started, pid; /* this session */
  struct pso_gen_manifest current; /* loaded at open, replaced by each publish */
  uint64_t next_generation;
  uint64_t published; /* generations this session published */
  uint8_t last_sha256[32]; /* the shard of the last publish that wrote one */
  /* tests only: fail publish step N (1 shard write, 2 shard sync, 3 shard rename, 4 manifest write,
     5 manifest rename, 6 index write, 7 index rename); 0: never */
  int fail_at;
};

/* The adapter writes one complete shard to `path`. NULL on success, else why (into `why`, or a
   static string). */
typedef const char *(*pso_gen_write_fn)(void *ctx, const char *path, char *why, size_t why_size);

/* Creates `dir` if needed, takes the writer lock if it can, loads the newest good generation and,
   as the writer, removes leftovers and unreferenced files. False (store unusable) only when the
   directory can't be created or opened. Reports what it loaded, dropped and removed. */
bool pso_gen_open(struct pso_gen_store *s, const char *dir, uint32_t keep_sessions, uint64_t started, uint64_t pid,
                  struct pso_report *r);
/* Releases the lock and the loaded manifest. */
void pso_gen_close(struct pso_gen_store *s);

/* The path of a shard of the store. */
void pso_gen_shard_path(const struct pso_gen_store *s, const struct pso_gen_shard *shard, char *buf, size_t size);

/* This session's shards in the current generation. */
uint64_t pso_gen_session_shards(const struct pso_gen_store *s);

/* Publishes a generation: a new shard of this session, written now by `write` (its persisted
   `keys`); this session's earlier shards unless `replace_session`; the usable shards of the newest
   keep_sessions - 1 other sessions of the current generation; and the current generation's excluded
   shapes plus `excluded`. With `write` NULL, an exclusion-only generation: no new shard (keys and
   replace_session ignored), every usable current shard carried (#89), the excluded shapes added. Writer
   only. NULL on success (the shard's size in *bytes), else why: the
   callback's error text as it gave it, or the step that failed. */
const char *pso_gen_publish(struct pso_gen_store *s, pso_gen_write_fn write, void *ctx, const uint64_t *keys,
                            uint64_t key_count, bool replace_session, const uint64_t *excluded,
                            uint64_t excluded_count, uint64_t *bytes, char *why, size_t why_size);

/* The manifest's bytes, and parsing them back (false: malformed or checksum mismatch). Exposed for
   the tests and the native tool. */
bool pso_gen_encode(const struct pso_gen_manifest *m, uint8_t **out, size_t *out_size);
bool pso_gen_decode(const uint8_t *data, size_t size, struct pso_gen_manifest *m);
void pso_gen_manifest_free(struct pso_gen_manifest *m);
