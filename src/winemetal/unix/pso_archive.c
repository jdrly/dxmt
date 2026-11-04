/* gw2 (#5): persistent Metal binary archives for pipelines (DXMT_PSO_ARCHIVE=1, off by default).
 *
 * The PE side calls WMTPSOArchiveInit once per process with the device. From then on every
 * render and compute pipeline the bridge builds (whose caller didn't set archives itself):
 *  - is first looked up in the archives of earlier sessions, with
 *    MTLPipelineOptionFailOnBinaryArchiveMiss, so a hit and a miss can be counted; a miss is
 *    built again the normal way;
 *  - is then added to this session's archive, on a serial background queue (utility QoS), so the
 *    extra backend work never delays the pipeline the game waits for.
 * The PE side serializes this session's archive every DXMT_PSO_FLUSH_EVERY new pipelines (on its
 * own thread, WMTPSOArchiveFlush) and every 30 s if any are new. Exit never flushes; it only
 * reports what the last flush missed.
 *
 * Rules carried over from d9mt's Metal 4 archives (work/d9mt-ref, docs/state-cache.md §6):
 *  - newBinaryArchiveWithDescriptor: with a URL crashes on a file that isn't an archive and may
 *    accept a truncated one, so every archive has a .sum next to it ("<size> <sha256>") written
 *    after it, and only an archive whose .sum matches is loaded; anything else is deleted;
 *  - writes go to .tmp files that are renamed into place;
 *  - archives live per executable, OS build and GPU:
 *    <user cache dir>/dxmt/pso/<exe>/os-<build>-<gpu>/ (DXMT_PSO_ARCHIVE_DIR replaces
 *    <user cache dir>/dxmt/pso); other os-* directories of the executable are removed;
 *  - the newest keep_sessions sessions are kept. A session's archive holds every pipeline that
 *    session built, hits included, so dropping old sessions loses nothing still in use.
 * MTLBinaryArchive serializes all of its contents each time, so each session has one file,
 * <unix time>-<pid>.mtlarchive, rewritten on every flush.
 *
 * gw2 (#52): every pipeline added is recorded (descriptor, stable key, compute or render) so that
 *  - a flush that fails is retried once into a fresh archive rebuilt from the records, without the
 *    pipeline Metal's error names ("pipeline no. N"), whose shape is logged; a pipeline left out
 *    that way stays out (counted as `skipped`);
 *  - each successful flush writes <archive>.keys next to it (the keys of the pipelines in it), and
 *    a lookup miss is classified against the loaded sessions' keys: `known` (archived before, yet
 *    Metal missed it, or it was lost before a flush) or `new` (never archived).
 * A key is FNV-1a 64 over the function names (which carry content hashes: <stage>_<shader sha1
 * prefix>_<variant digest>) and every descriptor field the bridge sets.
 *
 * gw2 (#75): the archive is an instance (struct pso_archive) with three separate parts: its
 * persistence state (what is captured, recorded, loaded and written), its telemetry (the counters
 * WMTPSOArchiveQuery reports; no decision reads them) and this file's Metal 3 adapter
 * (MTLBinaryArchive, the device, the add queue). The bridge acts on one process instance; the
 * native archive tool can open its own. What to keep, what a failed flush skips and how a miss is
 * classified is the pure policy's (pso_archive_policy.h). With DXMT_PSO_GENERATIONS=1 (config
 * `generations`) the session file and its .sum/.keys are replaced by the generation publisher
 * (pso_archive_publish.h: immutable shards, checksummed manifests, one atomically replaced index,
 * the last good generation kept) in <os dir>/gen1. Each flush publishes the pipelines added since
 * the last one as a shard of their own (a delta; after kSessionShards of them, one merged shard of
 * the whole session), from an archive never serialized before: Metal fails to serialize an archive
 * again once pipelines were added after a serialization, naming a good pipeline (reproduced by
 * tests/gw2/pso_archive_test.m; the session file mode then drops that pipeline). The exact-shape
 * failure policy excludes a key Metal names in such a fresh archive from capture, now and in later
 * sessions. Without the switch everything is as #52 left it.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "pso_archive.h"
#include "pso_archive_policy.h"
#include "pso_archive_publish.h"

/* gw2 (#75): the counters WMTPSOArchiveQuery reports. Written where they always were; read only by
   reports and the query, never by a persistence decision. */
struct pso_telemetry {
  _Atomic uint64_t hits, misses, added, add_failed, capped, queued, unflushed, flushes, flushed_bytes;
  /* added when the last successful flush began: what is on disk (for the exit report) */
  _Atomic uint64_t persisted;
  _Atomic uint64_t miss_known, miss_new, skipped, rebuilds; /* gw2 (#52) */
  _Atomic uint64_t miss_excluded, excluded, generation, published; /* gw2 (#75), generations only */
};

/* gw2 (#75): what is captured, recorded, loaded and written. */
struct pso_persist {
  /* set by pso_archive_open, immutable after it */
  NSArray *lookup; /* id<MTLBinaryArchive> of earlier sessions */
  uint64_t loaded;
  struct pso_keyset known;          /* the loaded archives' persisted keys */
  struct pso_keyset excluded_prior; /* failing shapes of earlier generations */
  struct pso_policy policy;
  bool generations;                 /* the generation publisher instead of the session file */
  bool capture;                     /* false: lookup only (another process writes the store) */
  NSString *path;                   /* the session file (legacy) */
  struct pso_gen_store store;       /* generations */
  _Atomic uint64_t reserved;        /* captures admitted so far (the session cap) */
  /* under lock: adds and writes */
  pthread_mutex_t lock;
  id<MTLBinaryArchive> current;
  struct pso_records records;       /* the session's pipelines and current's order */
  bool dirty;                       /* current still holds a pipeline marked skipped */
  struct pso_keyset excluded_session;
  uint64_t pending;                 /* added since the last successful write */
  char add_error[256];
  /* gw2 (#75), generations: records before this index are in published shards; current holds the
     rest. Metal fails to serialize an archive again once more pipelines were added after a
     serialization (it names a good pipeline), so current is serialized at most once: after that a
     write rebuilds a fresh archive */
  uint64_t published_upto;
  bool serialized;
};

struct pso_archive {
  _Atomic bool enabled;
  struct pso_persist persist;
  struct pso_telemetry stats;
  /* the Metal 3 adapter */
  id<MTLDevice> device;
  dispatch_queue_t queue;
};

/* the process instance: the bridge calls act on it */
static struct pso_archive g_archive = {.persist = {.lock = PTHREAD_MUTEX_INITIALIZER}};

struct pso_archive *
pso_archive_process(void) {
  return &g_archive;
}

struct pso_archive *
pso_archive_create(void) {
  struct pso_archive *a = calloc(1, sizeof(*a));
  if (a)
    pthread_mutex_init(&a->persist.lock, NULL);
  return a;
}

static uint64_t
fnv(uint64_t h, const void *p, size_t n) {
  const uint8_t *b = p;
  for (size_t i = 0; i < n; i++)
    h = (h ^ b[i]) * 0x100000001b3ull;
  return h;
}
#define FNV_U(h, v)                                                                                                    \
  do {                                                                                                                 \
    uint64_t v_ = (uint64_t)(v);                                                                                       \
    h = fnv(h, &v_, sizeof(v_));                                                                                       \
  } while (0)

static uint64_t
fnv_str(uint64_t h, NSString *s) {
  const char *u = s ? s.UTF8String : "";
  return fnv(h, u, strlen(u) + 1);
}

/* a function's identity: its name, or the label the bridge gives a function made with constant
   values ("<name>#c<hash of the values>", _MTLLibrary_newFunctionWithConstants) */
static NSString *
pso_fn_id(id<MTLFunction> fn) {
  if (!fn)
    return nil;
  NSString *label = fn.label;
  return label.length ? label : fn.name;
}

/* the stable key of a pipeline descriptor (render or compute), and optionally its shape */
static uint64_t
pso_key(id desc, bool compute, char *shape, size_t shape_size) {
  uint64_t h = 0xcbf29ce484222325ull;
  if (compute) {
    MTLComputePipelineDescriptor *d = desc;
    h = fnv(h, "C", 1);
    h = fnv_str(h, pso_fn_id(d.computeFunction));
    FNV_U(h, d.threadGroupSizeIsMultipleOfThreadExecutionWidth);
    FNV_U(h, d.supportIndirectCommandBuffers);
    for (unsigned i = 0; i < 31; i++)
      FNV_U(h, d.buffers[i].mutability);
    if (shape)
      snprintf(shape, shape_size, "compute cs=%s", pso_fn_id(d.computeFunction).UTF8String ?: "-");
  } else {
    MTLRenderPipelineDescriptor *d = desc;
    h = fnv(h, "R", 1);
    h = fnv_str(h, pso_fn_id(d.vertexFunction));
    h = fnv_str(h, pso_fn_id(d.fragmentFunction));
    /* logic operation: only through private selectors; included when the descriptor has them */
    if ([d respondsToSelector:@selector(logicOperationEnabled)] && [d respondsToSelector:@selector(logicOperation)]) {
      FNV_U(h, [[(id)d valueForKey:@"logicOperationEnabled"] unsignedLongLongValue]);
      FNV_U(h, [[(id)d valueForKey:@"logicOperation"] unsignedLongLongValue]);
    }
    char colors[96] = "";
    size_t cl = 0;
    for (unsigned i = 0; i < 8; i++) {
      MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
      FNV_U(h, c.pixelFormat);
      FNV_U(h, c.blendingEnabled);
      FNV_U(h, c.writeMask);
      FNV_U(h, c.rgbBlendOperation);
      FNV_U(h, c.alphaBlendOperation);
      FNV_U(h, c.sourceRGBBlendFactor);
      FNV_U(h, c.sourceAlphaBlendFactor);
      FNV_U(h, c.destinationRGBBlendFactor);
      FNV_U(h, c.destinationAlphaBlendFactor);
      if (shape && c.pixelFormat != MTLPixelFormatInvalid && cl < sizeof(colors) - 12)
        cl += snprintf(colors + cl, sizeof(colors) - cl, "%s%u:%lu", cl ? "," : "", i, (unsigned long)c.pixelFormat);
    }
    FNV_U(h, d.depthAttachmentPixelFormat);
    FNV_U(h, d.stencilAttachmentPixelFormat);
    FNV_U(h, d.rasterSampleCount);
    FNV_U(h, d.alphaToCoverageEnabled);
    FNV_U(h, d.rasterizationEnabled);
    FNV_U(h, d.inputPrimitiveTopology);
    FNV_U(h, d.supportIndirectCommandBuffers);
    FNV_U(h, d.maxTessellationFactor);
    FNV_U(h, d.tessellationPartitionMode);
    FNV_U(h, d.tessellationFactorStepFunction);
    FNV_U(h, d.tessellationOutputWindingOrder);
    for (unsigned i = 0; i < 31; i++) {
      FNV_U(h, d.vertexBuffers[i].mutability);
      FNV_U(h, d.fragmentBuffers[i].mutability);
    }
    if (shape)
      snprintf(shape, shape_size, "render vs=%s fs=%s colors=[%s] depth=%lu stencil=%lu samples=%lu raster=%d topology=%lu",
               pso_fn_id(d.vertexFunction).UTF8String ?: "-", pso_fn_id(d.fragmentFunction).UTF8String ?: "none", colors,
               (unsigned long)d.depthAttachmentPixelFormat, (unsigned long)d.stencilAttachmentPixelFormat,
               (unsigned long)d.rasterSampleCount, (int)d.rasterizationEnabled, (unsigned long)d.inputPrimitiveTopology);
  }
  return h ? h : 1;
}

#define report_line pso_report_line

static bool
pso_instance_enabled(struct pso_archive *a) {
  return atomic_load_explicit(&a->enabled, memory_order_acquire);
}

bool
pso_archive_enabled(void) {
  return pso_instance_enabled(&g_archive);
}

/* "<size> <sha256 hex>" of a file, or nil. */
static NSString *
pso_sum_text(NSString *path, uint64_t *size_out) {
  NSData *d = [[NSData alloc] initWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
  if (!d)
    return nil;
  CC_SHA256_CTX ctx;
  CC_SHA256_Init(&ctx);
  const uint8_t *p = d.bytes;
  for (NSUInteger left = d.length; left;) {
    CC_LONG chunk = left > 0x40000000u ? 0x40000000u : (CC_LONG)left;
    CC_SHA256_Update(&ctx, p, chunk);
    p += chunk;
    left -= chunk;
  }
  unsigned char h[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(h, &ctx);
  NSMutableString *s = [NSMutableString stringWithFormat:@"%llu ", (unsigned long long)d.length];
  for (unsigned i = 0; i < sizeof(h); i++)
    [s appendFormat:@"%02x", h[i]];
  if (size_out)
    *size_out = d.length;
  [d release];
  return s;
}

/* NULL when the archive's .sum matches, else why not. */
static const char *
pso_archive_invalid(NSString *path, uint64_t *size_out) {
  NSString *want = [NSString stringWithContentsOfFile:[path stringByAppendingString:@".sum"]
                                             encoding:NSUTF8StringEncoding
                                                error:nil];
  if (!want)
    return "no .sum";
  NSString *have = pso_sum_text(path, size_out);
  if (!have)
    return "unreadable";
  if (![have isEqualToString:want])
    return ".sum does not match (truncated or interrupted write)";
  return NULL;
}

static void
pso_remove_archive(NSString *path) {
  unlink(path.fileSystemRepresentation);
  unlink([path stringByAppendingString:@".sum"].fileSystemRepresentation);
  unlink([path stringByAppendingString:@".keys"].fileSystemRepresentation); /* gw2 (#52) */
}

static NSString *
pso_sanitize(NSString *s) {
  char out[49];
  NSUInteger n = 0;
  for (; n < s.length && n < 48; n++) {
    unichar c = [s characterAtIndex:n];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
    out[n] = ok ? (char)c : '_';
  }
  out[n] = '\0';
  return [NSString stringWithUTF8String:out];
}

static double
mib(uint64_t bytes) {
  return (double)bytes / (1024.0 * 1024.0);
}

/* gw2 (#52): <archive>.keys: "DXMTKEY1", a uint64 count, then the keys; written after the archive
 * and its .sum, through a .tmp. Advisory only: a missing or short file just classifies fewer misses. */
static void
pso_write_keys(const struct pso_records *rec, NSString *path) {
  NSString *kp = [path stringByAppendingString:@".keys"];
  NSString *kt = [kp stringByAppendingString:@".tmp"];
  FILE *f = fopen(kt.fileSystemRepresentation, "wb");
  if (!f)
    return;
  uint64_t n = pso_records_kept_keys(rec, 0, NULL);
  bool ok = fwrite("DXMTKEY1", 8, 1, f) == 1 && fwrite(&n, sizeof(n), 1, f) == 1;
  for (uint64_t i = 0; ok && i < rec->count; i++)
    if (!rec->rec[i].skipped)
      ok = fwrite(&rec->rec[i].key, sizeof(uint64_t), 1, f) == 1;
  ok = !fclose(f) && ok;
  if (!ok || rename(kt.fileSystemRepresentation, kp.fileSystemRepresentation))
    unlink(kt.fileSystemRepresentation);
}

/* appends the keys of <archive>.keys to `out` (grown with realloc); the number read */
static uint64_t
pso_read_keys(NSString *path, uint64_t **out, uint64_t *count) {
  FILE *f = fopen([path stringByAppendingString:@".keys"].fileSystemRepresentation, "rb");
  if (!f)
    return 0;
  char magic[8];
  uint64_t n = 0, got = 0;
  if (fread(magic, 8, 1, f) == 1 && !memcmp(magic, "DXMTKEY1", 8) && fread(&n, sizeof(n), 1, f) == 1 &&
      n < (1u << 24)) {
    uint64_t *grown = realloc(*out, (*count + n) * sizeof(uint64_t));
    if (grown) {
      *out = grown;
      got = fread(*out + *count, sizeof(uint64_t), n, f);
      *count += got;
    }
  }
  fclose(f);
  return got;
}

/* the native harness (GW2 on Mac work/dxmt-squeeze/native, tests/gw2/pso_archive_*_test.m) fakes
   Metal's packer error: the next PSO_ARCHIVE_TEST_FAIL writes fail; NULL when this one doesn't */
static const char *
pso_test_fault(char *why, size_t why_size) {
#ifdef PSO_ARCHIVE_TESTING
  static int fake_failures = -1, fake_after = -1;
  if (fake_after < 0) /* gw2 (#75): writes that succeed before the fake failures begin */
    fake_after = getenv("PSO_ARCHIVE_TEST_AFTER") ? atoi(getenv("PSO_ARCHIVE_TEST_AFTER")) : 0;
  if (fake_after > 0) {
    fake_after--;
    return NULL;
  }
  if (fake_failures < 0)
    fake_failures = getenv("PSO_ARCHIVE_TEST_FAIL") ? atoi(getenv("PSO_ARCHIVE_TEST_FAIL")) : 0;
  if (fake_failures > 0) {
    fake_failures--;
    snprintf(why, why_size, "Failed to generate machO:error: cannot pack (test): expecting 'fragment' stage in "
             "pipeline no. '%s'", getenv("PSO_ARCHIVE_TEST_INDEX") ?: "3");
    return why;
  }
#else
  (void)why;
  (void)why_size;
#endif
  return NULL;
}

/* Serializes `archive` to the session file through .tmp and writes the .sum; NULL on success, else
   why. */
static const char *
pso_write_archive(struct pso_archive *a, id<MTLBinaryArchive> archive, uint64_t *size, char *why, size_t why_size) {
  NSString *path = a->persist.path;
  NSString *sum_path = [path stringByAppendingString:@".sum"];
  NSString *tmp = [path stringByAppendingString:@".tmp"];
  NSString *sum_tmp = [path stringByAppendingString:@".sum.tmp"];
  NSError *err = nil;
  const char *failed = pso_test_fault(why, why_size);
  if (failed)
    return failed;
  if (![archive serializeToURL:[NSURL fileURLWithPath:tmp] error:&err]) {
    snprintf(why, why_size, "%s", err ? err.localizedDescription.UTF8String : "serializeToURL failed");
    failed = why;
  } else {
    NSString *sum = pso_sum_text(tmp, size);
    if (!sum)
      failed = "cannot read the serialized archive back";
    else if (![sum writeToFile:sum_tmp atomically:NO encoding:NSUTF8StringEncoding error:nil])
      failed = "cannot write the .sum";
    else if (rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation))
      failed = "rename failed"; /* the previous flush's pair is still intact */
    else if (rename(sum_tmp.fileSystemRepresentation, sum_path.fileSystemRepresentation)) {
      failed = "rename of the .sum failed";
      pso_remove_archive(path); /* new archive, old .sum: never leave a mismatched pair */
    }
  }
  if (failed) {
    unlink(tmp.fileSystemRepresentation);
    unlink(sum_tmp.fileSystemRepresentation);
  }
  return failed;
}

/* gw2 (#75): the generation publisher's shard writer: one MTLBinaryArchive serialization */
static const char *
pso_write_shard(void *ctx, const char *path, char *why, size_t why_size) {
  id<MTLBinaryArchive> archive = ctx;
  const char *failed = pso_test_fault(why, why_size);
  if (failed)
    return failed;
  NSError *err = nil;
  if (![archive serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]] error:&err]) {
    snprintf(why, why_size, "%s", err ? err.localizedDescription.UTF8String : "serializeToURL failed");
    return why;
  }
  return NULL;
}

/* gw2 (#75): publishes `archive`, holding the records from `from` on that aren't skipped, as a new
   generation (`merge`: replacing this session's earlier shards); nil: an exclusion-only generation
   (the current shards and this session's excluded shapes) */
static const char *
pso_publish(struct pso_archive *a, id<MTLBinaryArchive> archive, uint64_t from, bool merge, uint64_t *size,
            char *why, size_t why_size) {
  struct pso_persist *p = &a->persist;
  uint64_t n = pso_records_kept_keys(&p->records, from, NULL);
  uint64_t *keys = n ? malloc(n * sizeof(uint64_t)) : NULL;
  uint64_t ex_n = 0;
  uint64_t *ex = p->excluded_session.count ? malloc(p->excluded_session.count * sizeof(uint64_t)) : NULL;
  if ((n && !keys) || (p->excluded_session.count && !ex)) {
    free(keys);
    free(ex);
    return "out of memory for the generation's keys";
  }
  pso_records_kept_keys(&p->records, from, keys);
  for (uint64_t i = 0; ex && i <= p->excluded_session.mask; i++)
    if (p->excluded_session.slots[i])
      ex[ex_n++] = p->excluded_session.slots[i];
#ifdef PSO_ARCHIVE_TESTING
  /* tests/gw2/pso_archive_generations.zsh: PSO_GEN_TEST_FAIL_AT=<step>:<call> fails this
     instance's <call>th publish (1-based) at the publisher's <step> */
  static int fail_step = -1, fail_call, calls;
  if (fail_step < 0) {
    const char *v = getenv("PSO_GEN_TEST_FAIL_AT");
    fail_step = v ? atoi(v) : 0;
    fail_call = v && strchr(v, ':') ? atoi(strchr(v, ':') + 1) : 1;
  }
  p->store.fail_at = ++calls == fail_call ? fail_step : 0;
#endif
  const char *failed = pso_gen_publish(&p->store, archive ? pso_write_shard : NULL, archive, keys, n, merge, ex, ex_n,
                                       size, why, why_size);
  free(keys);
  free(ex);
  return failed;
}

/* gw2 (#52): a fresh archive with every recorded pipeline that isn't skipped, and its order (`map`,
 * record indices). nil if it can't be made or if any re-add fails: a partial archive must never
 * replace the current one, whose keys and counts claim every recorded pipeline. Adding recompiles
 * each pipeline's backend code, served mostly by Metal's shader cache. */
static id<MTLBinaryArchive>
pso_rebuild(struct pso_archive *a, uint64_t from, uint64_t **map, uint64_t *map_count, uint64_t *map_cap,
            const char **why) {
  const struct pso_records *r = &a->persist.records;
  NSError *err = nil;
  MTLBinaryArchiveDescriptor *cd = [[MTLBinaryArchiveDescriptor alloc] init];
  id<MTLBinaryArchive> fresh = [a->device newBinaryArchiveWithDescriptor:cd error:&err];
  [cd release];
  *map_count = 0;
  if (!fresh) {
    *why = "cannot create an archive";
    return nil;
  }
  for (uint64_t i = from; i < r->count; i++) {
    if (r->rec[i].skipped)
      continue;
    BOOL ok = r->rec[i].compute ? [fresh addComputePipelineFunctionsWithDescriptor:r->rec[i].desc error:&err]
                                : [fresh addRenderPipelineFunctionsWithDescriptor:r->rec[i].desc error:&err];
    if (!ok || !pso_records_order_push(map, map_count, map_cap, i)) {
      *why = ok ? "out of memory" : "a pipeline could not be added again";
      [fresh release];
      return nil;
    }
  }
  return fresh;
}

/* gw2 (#52): what the policy leaves out after a write failed at position `idx` of an archive's
 * order (`map`); the named pipeline's shape into `shape`; false if `idx` isn't a recorded pipeline */
static bool
pso_skip_named(struct pso_archive *a, long idx, const uint64_t *map, uint64_t map_count, char *shape,
               size_t shape_size) {
  struct pso_persist *p = &a->persist;
  uint64_t newly = 0;
  int64_t named =
      pso_policy_skip(&p->policy, &p->records, map, map_count, idx, &p->excluded_session, &newly); /* gw2 (#75) */
  if (named < 0)
    return false;
  struct pso_record *rec = &p->records.rec[named];
  pso_key(rec->desc, rec->compute, shape, shape_size);
  atomic_fetch_add(&a->stats.skipped, newly);
  p->dirty = true;
  return true;
}

/* Caller holds the lock. 1: written, 0: nothing new, -1: failed (retried on the next flush). */
static int
pso_flush_locked(struct pso_archive *a, struct pso_report *r) {
  struct pso_persist *p = &a->persist;
  uint64_t n = p->pending;
  atomic_store(&a->stats.unflushed, 0);
  uint64_t added = atomic_load(&a->stats.added);
  if (p->add_error[0]) {
    report_line(r, "PSO archive: %llu pipelines could not be added so far; first error: %s",
                (unsigned long long)atomic_load(&a->stats.add_failed), p->add_error);
    p->add_error[0] = '\0';
  }
  if (!n)
    return 0;
  const char *name = p->path.lastPathComponent.UTF8String;
  uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  uint64_t size = 0;
  char why[1024];
  /* an earlier failed retry left skipped pipelines in the current archive: don't publish them,
     rebuild */
  const char *failed = p->dirty ? "the current archive still holds pipelines left out after an earlier failure"
                                : pso_write_archive(a, p->current, &size, why, sizeof(why));
  const char *retried = "";
  char retry_note[2048] = "";
  if (failed) {
    /* gw2 (#52): say which pipeline Metal named (in the current archive's own order), then rebuild
     * without it and try once more. A failing retry's named pipeline is skipped too, for the next
     * flush. */
    bool dirty = p->dirty;
    long idx = dirty ? -1 : pso_policy_failed_index(failed);
    char shape[512] = "not a recorded pipeline";
    pso_skip_named(a, idx, p->records.map, p->records.map_count, shape, sizeof(shape));
    if (dirty)
      report_line(r, "PSO archive: rebuilding %s without the pipelines left out after an earlier failure", name);
    else
      report_line(r, "PSO archive: flush of %s failed: %s", name, failed);
    if (idx >= 0)
      report_line(r, "PSO archive: Metal names pipeline no. %ld: %s (left out of the archive from now on)", idx, shape);
    uint64_t *fmap = NULL, fcount = 0, fcap = 0;
    const char *rebuild_why = "no device";
    id<MTLBinaryArchive> fresh = a->device ? pso_rebuild(a, 0, &fmap, &fcount, &fcap, &rebuild_why) : nil;
    char why2[1024];
    const char *failed2 = fresh ? pso_write_archive(a, fresh, &size, why2, sizeof(why2)) : rebuild_why;
    if (!failed2) {
      [p->current release];
      p->current = fresh;
      p->dirty = false;
      free(p->records.map);
      p->records.map = fmap;
      p->records.map_count = fcount;
      p->records.map_cap = fcap;
      atomic_fetch_add(&a->stats.rebuilds, 1);
      snprintf(retry_note, sizeof(retry_note), " (rebuilt with %llu pipelines after a failed flush)",
               (unsigned long long)fcount);
      retried = retry_note;
      failed = NULL;
    } else {
      long idx2 = fresh ? pso_policy_failed_index(failed2) : -1;
      char shape2[512] = "";
      if (fresh && pso_skip_named(a, idx2, fmap, fcount, shape2, sizeof(shape2)))
        report_line(r, "PSO archive: the rebuilt archive's pipeline no. %ld failed too: %s (left out from the next flush)",
                    idx2, shape2);
      [fresh release];
      free(fmap);
      snprintf(retry_note, sizeof(retry_note), "%s; the rebuild failed too: %s", failed, failed2);
      failed = retry_note;
    }
  }
  double ms = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
  if (failed) {
    atomic_fetch_add(&a->stats.unflushed, n);
    report_line(r, "PSO archive: flush of %s failed after %.0f ms: %s", name, ms, failed);
    return -1;
  }
  p->pending = 0;
  pso_write_keys(&p->records, p->path);
  atomic_fetch_add(&a->stats.flushes, 1);
  atomic_store(&a->stats.flushed_bytes, size);
  atomic_store(&a->stats.persisted, added);
  report_line(r, "PSO archive: flushed %s: %llu pipelines (+%llu), %.1f MB in %.0f ms%s%s", name,
              (unsigned long long)atomic_load(&a->stats.added), (unsigned long long)n, mib(size), ms,
              atomic_load(&a->stats.capped) ? " (session cap reached, later pipelines not added)" : "", retried);
  return 1;
}

/* gw2 (#75), generations: a session's shards before a publish merges them into one, and the writes
   one flush tries (each failure that names a pipeline excludes that shape and tries again) */
enum { kSessionShards = 8, kPublishAttempts = 3 };

/* this session excluded a shape the current generation doesn't record yet */
static bool
pso_new_exclusions(const struct pso_persist *p) {
  const struct pso_keyset *ex = &p->excluded_session;
  for (uint64_t i = 0; ex->slots && i <= ex->mask; i++) {
    bool recorded = false;
    for (uint64_t j = 0; ex->slots[i] && !recorded && j < p->store.current.excluded_count; j++)
      recorded = p->store.current.excluded[j] == ex->slots[i];
    if (ex->slots[i] && !recorded)
      return true;
  }
  return false;
}

static void
pso_hex12(const uint8_t *sha, char out[13]) {
  for (unsigned i = 0; i < 6; i++)
    snprintf(out + i * 2, 3, "%02x", sha[i]);
}

/* gw2 (#75): the flush with the generation publisher. Caller holds the lock; as pso_flush_locked.
 * Publishes the pipelines since the last publish as a new shard (or, once the session has
 * kSessionShards, all of its pipelines as one shard replacing them). The archive written is always
 * one that was never serialized before: current the first time, else a fresh rebuild (see
 * persist.serialized). A failure naming a pipeline is a failing shape of a fresh archive: the
 * exact-shape policy excludes its key, and the next attempt rebuilds without it. Any other failure
 * excludes nothing and leaves the last good generation; the next flush tries again. */
static int
pso_flush_generations_locked(struct pso_archive *a, struct pso_report *r) {
  struct pso_persist *p = &a->persist;
  uint64_t n = p->pending;
  atomic_store(&a->stats.unflushed, 0);
  uint64_t added = atomic_load(&a->stats.added);
  if (p->add_error[0]) {
    report_line(r, "PSO archive: %llu pipelines could not be added so far; first error: %s",
                (unsigned long long)atomic_load(&a->stats.add_failed), p->add_error);
    p->add_error[0] = '\0';
  }
  if (!n)
    return 0;
  unsigned long long gen = p->store.next_generation;
  uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  uint64_t merged = pso_gen_session_shards(&p->store);
  bool merge = merged >= kSessionShards;
  uint64_t from = merge ? 0 : p->published_upto;
  uint64_t size = 0, named = 0;
  char why[1024];
  const char *failed = NULL;
  bool exclusion_only = false;
  uint64_t generation_before = p->store.current.generation;
  for (int attempt = 0; attempt < kPublishAttempts; attempt++) {
    if (!pso_records_kept_keys(&p->records, from, NULL)) {
      /* everything new is an excluded shape: no shard, but the exclusions must reach the store
         before pending is cleared, or a later session captures them again */
      exclusion_only = true;
      size = 0;
      failed = pso_new_exclusions(p) ? pso_publish(a, nil, from, false, &size, why, sizeof(why)) : NULL;
      if (failed)
        report_line(r, "PSO archive: publish of generation %llu failed: %s", gen, failed);
      break;
    }
    bool fresh = merge || p->serialized || p->dirty;
    id<MTLBinaryArchive> archive = p->current;
    const uint64_t *map = p->records.map;
    uint64_t map_count = p->records.map_count;
    uint64_t *fmap = NULL, fcount = 0, fcap = 0;
    if (fresh) {
      const char *rebuild_why = "no device";
      archive = a->device ? pso_rebuild(a, from, &fmap, &fcount, &fcap, &rebuild_why) : nil;
      if (!archive) {
        free(fmap);
        failed = rebuild_why;
        break;
      }
      map = fmap;
      map_count = fcount;
    } else {
      p->serialized = true;
    }
    failed = pso_publish(a, archive, from, merge, &size, why, sizeof(why));
    long idx = failed ? pso_policy_failed_index(failed) : -1;
    char shape[512] = "";
    bool excluded = failed && pso_skip_named(a, idx, map, map_count, shape, sizeof(shape));
    if (fresh)
      [archive release];
    free(fmap);
    if (!failed)
      break;
    report_line(r, "PSO archive: publish of generation %llu failed: %s", gen, failed);
    if (!excluded)
      break;
    named++;
    report_line(r, "PSO archive: Metal names pipeline no. %ld of an archive never serialized before: %s (its key is "
                "excluded from capture from now on)",
                idx, shape);
  }
  double ms = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
  if (failed) {
    atomic_fetch_add(&a->stats.unflushed, n);
    report_line(r, "PSO archive: publish of generation %llu failed after %.0f ms: %s (generation %llu stays)", gen, ms,
                failed, (unsigned long long)p->store.current.generation);
    return -1;
  }
  uint64_t kept = exclusion_only ? 0 : pso_records_kept_keys(&p->records, from, NULL);
  bool published = p->store.current.generation != generation_before;
  p->pending = 0;
  p->published_upto = p->records.count;
  p->dirty = false;
  /* the next delta goes into an archive never serialized */
  MTLBinaryArchiveDescriptor *cd = [[MTLBinaryArchiveDescriptor alloc] init];
  id<MTLBinaryArchive> next = [a->device newBinaryArchiveWithDescriptor:cd error:nil];
  [cd release];
  if (next) {
    [p->current release];
    p->current = next;
    p->serialized = false;
    p->records.map_count = 0;
  } else {
    p->serialized = true; /* keep adding to the old one; the next flush rebuilds */
  }
  atomic_store(&a->stats.persisted, added);
  if (named)
    atomic_fetch_add(&a->stats.rebuilds, 1);
  if (!published) {
    report_line(r, "PSO archive: nothing to publish: the %llu new pipelines are all excluded shapes",
                (unsigned long long)n);
    return 0;
  }
  if (exclusion_only) {
    atomic_fetch_add(&a->stats.flushes, 1);
    atomic_store(&a->stats.generation, p->store.current.generation);
    atomic_store(&a->stats.published, 0);
    report_line(r, "PSO archive: published generation %llu: no shard, the %llu new pipelines are all excluded shapes; "
                "%llu shards, %llu excluded shapes, in %.0f ms",
                (unsigned long long)p->store.current.generation, (unsigned long long)n,
                (unsigned long long)p->store.current.shard_count, (unsigned long long)p->store.current.excluded_count,
                ms);
    return 1;
  }
  atomic_fetch_add(&a->stats.flushes, 1);
  atomic_store(&a->stats.flushed_bytes, size);
  atomic_store(&a->stats.generation, p->store.current.generation);
  atomic_store(&a->stats.published, kept);
  const struct pso_gen_manifest *m = &p->store.current;
  char h[13] = "", note[160] = "";
  pso_hex12(p->store.last_sha256, h);
  if (merge)
    snprintf(note, sizeof(note), " (merged this session's %llu shards into one)", (unsigned long long)merged);
  if (named)
    snprintf(note + strlen(note), sizeof(note) - strlen(note), " (after excluding %llu failing shapes)",
             (unsigned long long)named);
  report_line(r, "PSO archive: published generation %llu: %llu pipelines (+%llu) in shard %s, %.1f MB, %llu shards, "
              "%llu excluded shapes, in %.0f ms%s%s",
              (unsigned long long)m->generation, (unsigned long long)kept, (unsigned long long)n, h, mib(size),
              (unsigned long long)m->shard_count, (unsigned long long)m->excluded_count, ms,
              atomic_load(&a->stats.capped) ? " (session cap reached, later pipelines not added)" : "", note);
  return 1;
}

static void
pso_report_atexit(void) {
  /* Exit does no Metal or disk work: serializing can take long, and the threads Metal depends on
   * may already be gone. The periodic flushes bound what is lost (at most DXMT_PSO_FLUSH_EVERY - 1
   * pipelines or 30 s); only report it. Counted against the last completed flush (one still
   * running counts as not done); an add in progress can count twice, hence "about". */
  struct pso_telemetry *t = &g_archive.stats;
  uint64_t persisted = atomic_load(&t->persisted); /* first: never above the added read after it */
  uint64_t n = atomic_load(&t->added) - persisted + atomic_load(&t->queued);
  if (n)
    fprintf(stderr,
            "dxmt: PSO archive: about %llu pipelines added since the last flush are not saved (no flush at exit)\n",
            (unsigned long long)n);
}

static void
pso_queue_add(struct pso_archive *a, id desc, bool compute, uint64_t key) {
  struct pso_persist *p = &a->persist;
  if (!p->capture) /* gw2 (#75): lookup only */
    return;
  if (pso_policy_admit(&p->policy, atomic_fetch_add(&p->reserved, 1)) == PSO_KEEP_CAPPED) {
    atomic_fetch_add(&a->stats.capped, 1);
    return;
  }
  atomic_fetch_add(&a->stats.queued, 1);
  [desc retain];
  dispatch_async(a->queue, ^{
    @autoreleasepool {
      NSError *err = nil;
      BOOL ok;
      pthread_mutex_lock(&p->lock);
      /* gw2 (#75): a failing shape (exact-shape policy) is not captured again */
      if (pso_policy_shape(&p->policy, &p->excluded_session, key) == PSO_KEEP_EXCLUDED) {
        atomic_fetch_add(&a->stats.excluded, 1);
        pthread_mutex_unlock(&p->lock);
        [desc release];
        atomic_fetch_sub(&a->stats.queued, 1);
        return;
      }
      /* gw2 (#52): room for its record and its place in the current order first, so a pipeline is
         never in the archive without both (a misaligned map would skip the wrong record) */
      bool room = pso_records_reserve(&p->records);
      if (!room)
        ok = NO;
      else if (compute)
        ok = [p->current addComputePipelineFunctionsWithDescriptor:desc error:&err];
      else
        ok = [p->current addRenderPipelineFunctionsWithDescriptor:desc error:&err];
      if (ok) {
        atomic_fetch_add(&a->stats.added, 1);
        atomic_fetch_add(&a->stats.unflushed, 1);
        p->pending++;
        pso_records_push(&p->records, key, [desc retain], compute);
      } else {
        atomic_fetch_add(&a->stats.add_failed, 1);
        if (!p->add_error[0])
          snprintf(p->add_error, sizeof(p->add_error), "%s",
                   !room ? "out of memory for the pipeline's record (not added)"
                         : err ? err.localizedDescription.UTF8String : "(no error)");
      }
      pthread_mutex_unlock(&p->lock);
    }
    [desc release];
    atomic_fetch_sub(&a->stats.queued, 1);
  });
}

/* a lookup miss of `key`, counted by the policy's class */
static void
pso_count_miss(struct pso_archive *a, uint64_t key) {
  atomic_fetch_add(&a->stats.misses, 1);
  switch (pso_policy_classify(&a->persist.policy, key)) {
  case PSO_MISS_KNOWN:
    atomic_fetch_add(&a->stats.miss_known, 1);
    break;
  case PSO_MISS_EXCLUDED:
    atomic_fetch_add(&a->stats.miss_excluded, 1);
    break;
  case PSO_MISS_NEW:
    atomic_fetch_add(&a->stats.miss_new, 1);
    break;
  }
}

id<MTLRenderPipelineState>
pso_archive_build_render(struct pso_archive *a, id<MTLDevice> device, MTLRenderPipelineDescriptor *desc,
                         NSError **err) {
  id<MTLRenderPipelineState> pso = nil;
  NSArray *lookup = a->persist.lookup;
  if (lookup.count) {
    desc.binaryArchives = lookup;
    @autoreleasepool {
      pso = [device newRenderPipelineStateWithDescriptor:desc
                                                 options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                              reflection:nil
                                                   error:nil];
    }
    desc.binaryArchives = nil;
  }
  uint64_t key = pso_key(desc, false, NULL, 0); /* gw2 (#52) */
  if (pso) {
    atomic_fetch_add(&a->stats.hits, 1);
  } else {
    pso_count_miss(a, key);
    pso = [device newRenderPipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:err];
  }
  if (pso)
    pso_queue_add(a, desc, false, key);
  return pso;
}

id<MTLComputePipelineState>
pso_archive_build_compute(struct pso_archive *a, id<MTLDevice> device, MTLComputePipelineDescriptor *desc,
                          NSError **err) {
  id<MTLComputePipelineState> pso = nil;
  NSArray *lookup = a->persist.lookup;
  if (lookup.count) {
    desc.binaryArchives = lookup;
    @autoreleasepool {
      pso = [device newComputePipelineStateWithDescriptor:desc
                                                  options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                               reflection:nil
                                                    error:nil];
    }
    desc.binaryArchives = nil;
  }
  uint64_t key = pso_key(desc, true, NULL, 0); /* gw2 (#52) */
  if (pso) {
    atomic_fetch_add(&a->stats.hits, 1);
  } else {
    pso_count_miss(a, key);
    pso = [device newComputePipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:err];
  }
  if (pso)
    pso_queue_add(a, desc, true, key);
  return pso;
}

id<MTLRenderPipelineState>
pso_archive_new_render(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **err) {
  return pso_archive_build_render(&g_archive, device, desc, err);
}

id<MTLComputePipelineState>
pso_archive_new_compute(id<MTLDevice> device, MTLComputePipelineDescriptor *desc, NSError **err) {
  return pso_archive_build_compute(&g_archive, device, desc, err);
}

/* the loaded archives' keys into the policy's `known` set (none: the set stays empty) */
static void
pso_known_from(struct pso_persist *p, const uint64_t *keys, uint64_t key_count) {
  if (key_count && pso_keyset_init(&p->known, key_count))
    for (uint64_t i = 0; i < key_count; i++)
      pso_keyset_insert(&p->known, keys[i]);
}

/* #52's session files: loads the newest keep_sessions archives in `dir` for lookup; the known keys */
static void
pso_open_sessions(struct pso_archive *a, id<MTLDevice> device, const struct WMTPSOArchiveConfig *cfg, NSString *dir,
                  time_t now, NSMutableArray *archives, uint64_t *loaded_bytes, uint64_t *key_count_out,
                  uint64_t *keyed_out, struct pso_report *r) {
  NSFileManager *fm = [NSFileManager defaultManager];
  /* Collect archives; drop leftovers of interrupted writes and orphaned .sum files. */
  NSMutableArray<NSString *> *names = [NSMutableArray array];
  unsigned leftovers = 0;
  for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
    NSString *path = [dir stringByAppendingPathComponent:name];
    if ([name hasSuffix:@".mtlarchive"]) {
      [names addObject:name];
    } else if ([name hasSuffix:@".tmp"]) {
      struct stat st;
      if (!stat(path.fileSystemRepresentation, &st) && now - st.st_mtime > 600) {
        unlink(path.fileSystemRepresentation);
        leftovers++;
      }
    } else if ([name hasSuffix:@".mtlarchive.sum"] || [name hasSuffix:@".mtlarchive.keys"]) {
      if (![fm fileExistsAtPath:[path stringByDeletingPathExtension]]) {
        unlink(path.fileSystemRepresentation);
        leftovers++;
      }
    }
  }
  if (leftovers)
    report_line(r, "PSO archive: removed %u leftover .tmp/.sum files", leftovers);
  /* "<10-digit unix time>-<pid>.mtlarchive": lexical order is age; newest first. */
  [names sortUsingComparator:^NSComparisonResult(NSString *x, NSString *y) { return [y compare:x]; }];

  uint64_t *keys = NULL, key_count = 0, keyed_archives = 0; /* gw2 (#52) */
  for (NSString *name in names) {
    NSString *path = [dir stringByAppendingPathComponent:name];
    if (archives.count >= cfg->keep_sessions) {
      pso_remove_archive(path);
      report_line(r, "PSO archive: deleted %s: older than the newest %u sessions", name.UTF8String,
                  cfg->keep_sessions);
      continue;
    }
    uint64_t size = 0;
    const char *why = pso_archive_invalid(path, &size);
    if (why) {
      pso_remove_archive(path);
      report_line(r, "PSO archive: deleted %s: %s", name.UTF8String, why);
      continue;
    }
    MTLBinaryArchiveDescriptor *ad = [[MTLBinaryArchiveDescriptor alloc] init];
    ad.url = [NSURL fileURLWithPath:path];
    NSError *err = nil;
    id<MTLBinaryArchive> arc = [device newBinaryArchiveWithDescriptor:ad error:&err];
    [ad release];
    if (!arc) {
      pso_remove_archive(path);
      report_line(r, "PSO archive: deleted %s: Metal refused it: %s", name.UTF8String,
                  err ? err.localizedDescription.UTF8String : "(no error)");
      continue;
    }
    [archives addObject:arc];
    [arc release];
    *loaded_bytes += size;
    keyed_archives += pso_read_keys(path, &keys, &key_count) > 0;
    report_line(r, "PSO archive: loaded %s (%.1f MB)", name.UTF8String, mib(size));
  }
  /* gw2 (#52): the loaded sessions' keys, for classifying misses */
  pso_known_from(&a->persist, keys, key_count);
  free(keys);
  a->persist.path = [[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%010llu-%d.mtlarchive",
                                                                                    (unsigned long long)now,
                                                                                    (int)getpid()]] retain];
  *key_count_out = key_count;
  *keyed_out = keyed_archives;
}

/* gw2 (#75): the generation store in <dir>/gen1: loads its newest good generation's shards */
static bool
pso_open_generations(struct pso_archive *a, id<MTLDevice> device, const struct WMTPSOArchiveConfig *cfg,
                     NSString *dir, time_t now, NSMutableArray *archives, uint64_t *loaded_bytes,
                     struct pso_report *r) {
  struct pso_persist *p = &a->persist;
  NSString *gdir = [dir stringByAppendingPathComponent:@"gen1"];
  if (!pso_gen_open(&p->store, gdir.fileSystemRepresentation, cfg->keep_sessions, (uint64_t)now,
                    (uint64_t)getpid(), r))
    return false;
  p->capture = p->store.role == PSO_GEN_WRITER;
  struct pso_gen_manifest *m = &p->store.current;
  uint64_t key_count = 0;
  for (uint64_t i = 0; i < m->shard_count; i++) {
    struct pso_gen_shard *sh = &m->shards[i];
    char path[PATH_MAX + 80], h[13] = "";
    pso_gen_shard_path(&p->store, sh, path, sizeof(path));
    for (unsigned j = 0; j < 6; j++)
      snprintf(h + j * 2, 3, "%02x", sh->sha256[j]);
    MTLBinaryArchiveDescriptor *ad = [[MTLBinaryArchiveDescriptor alloc] init];
    ad.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    NSError *err = nil;
    id<MTLBinaryArchive> arc = [device newBinaryArchiveWithDescriptor:ad error:&err];
    [ad release];
    if (!arc) {
      sh->usable = false; /* left out of the next generation, collected after it */
      report_line(r, "PSO archive: shard %s (session %llu-%llu) not used: Metal refused it: %s", h,
                  (unsigned long long)sh->started, (unsigned long long)sh->pid,
                  err ? err.localizedDescription.UTF8String : "(no error)");
      continue;
    }
    [archives addObject:arc];
    [arc release];
    *loaded_bytes += sh->size;
    key_count += sh->key_count;
    report_line(r, "PSO archive: loaded shard %s (session %llu-%llu, %.1f MB, %llu pipeline keys)", h,
                (unsigned long long)sh->started, (unsigned long long)sh->pid, mib(sh->size),
                (unsigned long long)sh->key_count);
  }
  if (key_count && pso_keyset_init(&p->known, key_count))
    for (uint64_t i = 0; i < m->shard_count; i++)
      for (uint64_t k = 0; m->shards[i].usable && k < m->shards[i].key_count; k++)
        pso_keyset_insert(&p->known, m->shards[i].keys[k]);
  if (m->excluded_count && pso_keyset_init(&p->excluded_prior, m->excluded_count))
    for (uint64_t i = 0; i < m->excluded_count; i++)
      pso_keyset_insert(&p->excluded_prior, m->excluded[i]);
  atomic_store(&a->stats.generation, m->generation);
  return true;
}

bool
pso_archive_open(struct pso_archive *a, id<MTLDevice> device, const struct WMTPSOArchiveConfig *cfg,
                 struct pso_report *r) {
  struct pso_persist *p = &a->persist;
  NSFileManager *fm = [NSFileManager defaultManager];
  const char *base_dir = cfg->base_dir.ptr;
  NSString *base;
  if (base_dir && *base_dir) {
    if (base_dir[0] != '/') {
      report_line(r, "PSO archive: off: DXMT_PSO_ARCHIVE_DIR must be an absolute unix path (%s)", base_dir);
      return false;
    }
    base = [NSString stringWithUTF8String:base_dir];
  } else {
    char buf[PATH_MAX];
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf))) {
      report_line(r, "PSO archive: off: no user cache directory");
      return false;
    }
    base = [[NSString stringWithUTF8String:buf] stringByAppendingPathComponent:@"dxmt/pso"];
  }
  const char *exe = cfg->exe_name.ptr;
  NSString *exe_dir = [base stringByAppendingPathComponent:pso_sanitize([NSString stringWithUTF8String:exe && *exe ? exe : "unknown"] ?: @"unknown")];

  char osbuild[64] = "unknown";
  size_t oslen = sizeof(osbuild);
  if (sysctlbyname("kern.osversion", osbuild, &oslen, NULL, 0))
    strcpy(osbuild, "unknown");
  NSString *leaf = [NSString stringWithFormat:@"os-%@-%@", pso_sanitize([NSString stringWithUTF8String:osbuild] ?: @"unknown"),
                                              pso_sanitize(device.name ?: @"gpu")];
  NSString *dir = [exe_dir stringByAppendingPathComponent:leaf];
  if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) {
    report_line(r, "PSO archive: off: cannot create %s", dir.UTF8String);
    return false;
  }
  for (NSString *name in [fm contentsOfDirectoryAtPath:exe_dir error:nil]) {
    if ([name hasPrefix:@"os-"] && ![name isEqualToString:leaf]) {
      [fm removeItemAtPath:[exe_dir stringByAppendingPathComponent:name] error:nil];
      report_line(r, "PSO archive: removed %s (another OS build or GPU)", name.UTF8String);
    }
  }

  time_t now = time(NULL);
  NSMutableArray *archives = [NSMutableArray array];
  uint64_t loaded_bytes = 0;
  uint64_t key_count = 0, keyed_archives = 0;
  p->generations = cfg->generations != 0; /* gw2 (#75) */
  p->capture = true;
  if (p->generations) {
    if (!pso_open_generations(a, device, cfg, dir, now, archives, &loaded_bytes, r))
      return false;
  } else {
    pso_open_sessions(a, device, cfg, dir, now, archives, &loaded_bytes, &key_count, &keyed_archives, r);
  }

  MTLBinaryArchiveDescriptor *cd = [[MTLBinaryArchiveDescriptor alloc] init];
  NSError *err = nil;
  p->current = [device newBinaryArchiveWithDescriptor:cd error:&err];
  [cd release];
  if (!p->current) {
    report_line(r, "PSO archive: off: cannot create this session's archive: %s",
                err ? err.localizedDescription.UTF8String : "(no error)");
    return false;
  }
  p->lookup = [archives copy];
  p->loaded = archives.count;
  p->policy.failure = p->generations ? PSO_SKIP_SHAPE : PSO_SKIP_RECORD; /* gw2 (#75) */
  p->policy.max_pipelines = cfg->max_pipelines;
  p->policy.known = &p->known;
  p->policy.excluded_prior = &p->excluded_prior;
  a->device = [device retain];
  a->queue = dispatch_queue_create("dxmt.pso-archive",
                                   dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
  if (a == &g_archive)
    atexit(pso_report_atexit);
  atomic_store_explicit(&a->enabled, true, memory_order_release);
  if (p->generations) {
    const struct pso_gen_manifest *m = &p->store.current;
    report_line(r, "PSO archive: on (generations%s): generation %llu, %lu shards for lookup (%.1f MB) in %s; %s "
                "(keep %u sessions, at most %u pipelines each)",
                p->capture ? "" : ", lookup only", (unsigned long long)m->generation, (unsigned long)archives.count,
                mib(loaded_bytes), p->store.dir,
                p->capture ? "this session publishes new generations" : "this session captures nothing",
                cfg->keep_sessions, cfg->max_pipelines);
    report_line(r, "PSO archive: %llu pipeline keys classify lookup misses (known/new), %llu failing shapes are "
                "excluded from capture",
                (unsigned long long)p->known.count, (unsigned long long)m->excluded_count);
  } else {
    report_line(r, "PSO archive: on: %lu archives for lookup (%.1f MB) in %s; this session writes %s (keep %u sessions, "
                "at most %u pipelines each)",
                (unsigned long)archives.count, mib(loaded_bytes), dir.UTF8String, p->path.lastPathComponent.UTF8String,
                cfg->keep_sessions, cfg->max_pipelines);
    report_line(r, "PSO archive: %llu pipeline keys from %llu of the loaded archives classify lookup misses (known/new)",
                (unsigned long long)key_count, (unsigned long long)keyed_archives);
  }
  return true;
}

int
pso_archive_flush(struct pso_archive *a, struct pso_report *rp) {
  if (!pso_instance_enabled(a))
    return -1;
  __block int ret = 0;
  /* Behind every add queued so far; only the calling thread waits. */
  dispatch_sync(a->queue, ^{
    @autoreleasepool {
      pthread_mutex_lock(&a->persist.lock);
      ret = a->persist.generations ? pso_flush_generations_locked(a, rp) : pso_flush_locked(a, rp);
      pthread_mutex_unlock(&a->persist.lock);
    }
  });
  return ret;
}

void
pso_archive_query(struct pso_archive *a, struct WMTPSOArchiveStats *s) {
  struct pso_telemetry *t = &a->stats;
  bool on = pso_instance_enabled(a);
  s->loaded = on ? a->persist.loaded : 0;
  s->lookup_hits = atomic_load(&t->hits);
  s->lookup_misses = atomic_load(&t->misses);
  s->added = atomic_load(&t->added);
  s->add_failed = atomic_load(&t->add_failed);
  s->capped = atomic_load(&t->capped);
  s->queued = atomic_load(&t->queued);
  s->unflushed = atomic_load(&t->unflushed);
  s->flushes = atomic_load(&t->flushes);
  s->flushed_bytes = atomic_load(&t->flushed_bytes);
  s->miss_known = atomic_load(&t->miss_known); /* gw2 (#52) */
  s->miss_new = atomic_load(&t->miss_new);
  s->skipped = atomic_load(&t->skipped);
  s->rebuilds = atomic_load(&t->rebuilds);
  /* gw2 (#75) */
  s->mode = !on || !a->persist.generations ? 0 : a->persist.capture ? 1 : 2;
  s->miss_excluded = atomic_load(&t->miss_excluded);
  s->excluded = atomic_load(&t->excluded);
  s->generation = atomic_load(&t->generation);
  s->published = atomic_load(&t->published);
}

int
_WMTPSOArchiveInit(void *obj) {
  struct unixcall_wmt_pso_archive_init *params = obj;
  static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
  static bool initialized;
  struct pso_report r = {params->report.ptr, params->report_size, 0};
  if (r.buf && r.size)
    r.buf[0] = '\0';
  pthread_mutex_lock(&init_lock);
  if (initialized) {
    params->ret = pso_archive_enabled();
  } else {
    initialized = true;
    @autoreleasepool {
      params->ret = pso_archive_open(&g_archive, (id<MTLDevice>)params->device, params->config.ptr, &r);
    }
  }
  pthread_mutex_unlock(&init_lock);
  return 0;
}

int
_WMTPSOArchiveFlush(void *obj) {
  struct unixcall_wmt_pso_archive_flush *params = obj;
  struct pso_report r = {params->report.ptr, params->report_size, 0};
  if (r.buf && r.size)
    r.buf[0] = '\0';
  params->ret = pso_archive_flush(&g_archive, &r);
  return 0;
}

int
_WMTPSOArchiveQuery(void *obj) {
  struct unixcall_wmt_pso_archive_query *params = obj;
  struct WMTPSOArchiveStats *s = params->stats.ptr;
  if (!s)
    return 0;
  pso_archive_query(&g_archive, s);
  return 0;
}
