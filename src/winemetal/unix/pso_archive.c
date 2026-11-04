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

static _Atomic bool g_enabled;
static NSArray *g_lookup; /* id<MTLBinaryArchive> of earlier sessions; immutable after init */
static id<MTLBinaryArchive> g_current;
static dispatch_queue_t g_queue;
/* Guards g_current (adds and serialization) and g_add_error. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static NSString *g_path;
static uint32_t g_max_pipelines;
static uint64_t g_loaded;
static char g_add_error[256];
static _Atomic uint64_t g_hits, g_misses, g_added, g_add_failed, g_capped, g_queued, g_unflushed;
static _Atomic uint64_t g_flushes, g_flushed_bytes, g_reserved;
/* g_added when the last successful flush began: what is on disk (adds and flushes hold g_lock). */
static _Atomic uint64_t g_persisted;

/* gw2 (#52): the session's added pipelines in add order (g_lock), and the loaded sessions' keys */
struct pso_record {
  uint64_t key;
  id desc; /* retained MTLRenderPipelineDescriptor or MTLComputePipelineDescriptor */
  bool compute;
  bool skipped; /* left out of the archive after Metal's packer named it */
};
static struct pso_record *g_rec;
static uint64_t g_rec_count, g_rec_cap;
static uint64_t *g_known;  /* open addressing, 0 = empty (a key of 0 is stored as 1) */
static uint64_t g_known_mask;
static _Atomic uint64_t g_miss_known, g_miss_new, g_skipped, g_rebuilds;

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

/* the stable key of a pipeline descriptor (render or compute), and optionally its shape */
static uint64_t
pso_key(id desc, bool compute, char *shape, size_t shape_size) {
  uint64_t h = 0xcbf29ce484222325ull;
  if (compute) {
    MTLComputePipelineDescriptor *d = desc;
    h = fnv(h, "C", 1);
    h = fnv_str(h, d.computeFunction.name);
    FNV_U(h, d.threadGroupSizeIsMultipleOfThreadExecutionWidth);
    FNV_U(h, d.supportIndirectCommandBuffers);
    for (unsigned i = 0; i < 31; i++)
      FNV_U(h, d.buffers[i].mutability);
    if (shape)
      snprintf(shape, shape_size, "compute cs=%s", d.computeFunction.name.UTF8String ?: "-");
  } else {
    MTLRenderPipelineDescriptor *d = desc;
    h = fnv(h, "R", 1);
    h = fnv_str(h, d.vertexFunction.name);
    h = fnv_str(h, d.fragmentFunction.name);
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
               d.vertexFunction.name.UTF8String ?: "-", d.fragmentFunction.name.UTF8String ?: "none", colors,
               (unsigned long)d.depthAttachmentPixelFormat, (unsigned long)d.stencilAttachmentPixelFormat,
               (unsigned long)d.rasterSampleCount, (int)d.rasterizationEnabled, (unsigned long)d.inputPrimitiveTopology);
  }
  return h ? h : 1;
}

static void
pso_known_insert(uint64_t key) {
  for (uint64_t i = key & g_known_mask;; i = (i + 1) & g_known_mask) {
    if (g_known[i] == key)
      return;
    if (!g_known[i]) {
      g_known[i] = key;
      return;
    }
  }
}

static bool
pso_known_has(uint64_t key) {
  if (!g_known)
    return false;
  for (uint64_t i = key & g_known_mask;; i = (i + 1) & g_known_mask) {
    if (g_known[i] == key)
      return true;
    if (!g_known[i])
      return false;
  }
}

struct report {
  char *buf;
  size_t size;
  size_t len;
};

static void report_line(struct report *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
report_line(struct report *r, const char *fmt, ...) {
  if (!r->buf || r->len + 2 >= r->size)
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

bool
pso_archive_enabled(void) {
  return atomic_load_explicit(&g_enabled, memory_order_acquire);
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
pso_write_keys(NSString *path) {
  NSString *kp = [path stringByAppendingString:@".keys"];
  NSString *kt = [kp stringByAppendingString:@".tmp"];
  FILE *f = fopen(kt.fileSystemRepresentation, "wb");
  if (!f)
    return;
  uint64_t n = 0;
  for (uint64_t i = 0; i < g_rec_count; i++)
    n += !g_rec[i].skipped;
  bool ok = fwrite("DXMTKEY1", 8, 1, f) == 1 && fwrite(&n, sizeof(n), 1, f) == 1;
  for (uint64_t i = 0; ok && i < g_rec_count; i++)
    if (!g_rec[i].skipped)
      ok = fwrite(&g_rec[i].key, sizeof(uint64_t), 1, f) == 1;
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

/* Serializes `archive` to g_path through .tmp and writes the .sum; NULL on success, else why. */
static const char *
pso_write_archive(id<MTLBinaryArchive> archive, uint64_t *size, char *why, size_t why_size) {
  NSString *sum_path = [g_path stringByAppendingString:@".sum"];
  NSString *tmp = [g_path stringByAppendingString:@".tmp"];
  NSString *sum_tmp = [g_path stringByAppendingString:@".sum.tmp"];
  NSError *err = nil;
  const char *failed = NULL;
#ifdef PSO_ARCHIVE_TESTING
  /* the native harness (GW2 on Mac work/dxmt-squeeze/native) fakes Metal's packer error once */
  static int fake_failures = -1;
  if (fake_failures < 0)
    fake_failures = getenv("PSO_ARCHIVE_TEST_FAIL") ? atoi(getenv("PSO_ARCHIVE_TEST_FAIL")) : 0;
  if (fake_failures > 0) {
    fake_failures--;
    snprintf(why, why_size, "Failed to generate machO:error: cannot pack (test): expecting 'fragment' stage in "
             "pipeline no. '%s'", getenv("PSO_ARCHIVE_TEST_INDEX") ?: "3");
    return why;
  }
#endif
  if (![archive serializeToURL:[NSURL fileURLWithPath:tmp] error:&err]) {
    snprintf(why, why_size, "%s", err ? err.localizedDescription.UTF8String : "serializeToURL failed");
    failed = why;
  } else {
    NSString *sum = pso_sum_text(tmp, size);
    if (!sum)
      failed = "cannot read the serialized archive back";
    else if (![sum writeToFile:sum_tmp atomically:NO encoding:NSUTF8StringEncoding error:nil])
      failed = "cannot write the .sum";
    else if (rename(tmp.fileSystemRepresentation, g_path.fileSystemRepresentation))
      failed = "rename failed"; /* the previous flush's pair is still intact */
    else if (rename(sum_tmp.fileSystemRepresentation, sum_path.fileSystemRepresentation)) {
      failed = "rename of the .sum failed";
      pso_remove_archive(g_path); /* new archive, old .sum: never leave a mismatched pair */
    }
  }
  if (failed) {
    unlink(tmp.fileSystemRepresentation);
    unlink(sum_tmp.fileSystemRepresentation);
  }
  return failed;
}

/* gw2 (#52): Metal's packer names the pipeline it choked on as "pipeline no. 'N'"; -1 if not */
static long
pso_failed_index(const char *why) {
  const char *p = strstr(why, "pipeline no. '");
  return p ? strtol(p + 14, NULL, 10) : -1;
}

/* gw2 (#52): a fresh archive with every recorded pipeline that isn't skipped; nil if it can't be
 * made. Adding recompiles each pipeline's backend code, served mostly by Metal's shader cache. */
static id<MTLBinaryArchive>
pso_rebuild(id<MTLDevice> device, uint64_t *readded) {
  NSError *err = nil;
  MTLBinaryArchiveDescriptor *cd = [[MTLBinaryArchiveDescriptor alloc] init];
  id<MTLBinaryArchive> fresh = [device newBinaryArchiveWithDescriptor:cd error:&err];
  [cd release];
  if (!fresh)
    return nil;
  uint64_t n = 0;
  for (uint64_t i = 0; i < g_rec_count; i++) {
    if (g_rec[i].skipped)
      continue;
    BOOL ok = g_rec[i].compute ? [fresh addComputePipelineFunctionsWithDescriptor:g_rec[i].desc error:&err]
                               : [fresh addRenderPipelineFunctionsWithDescriptor:g_rec[i].desc error:&err];
    n += ok;
  }
  *readded = n;
  return fresh;
}

static id<MTLDevice> g_device;

/* Caller holds g_lock. 1: written, 0: nothing new, -1: failed (retried on the next flush). */
static int
pso_flush_locked(struct report *r) {
  uint64_t n = atomic_exchange(&g_unflushed, 0);
  uint64_t added = atomic_load(&g_added);
  if (g_add_error[0]) {
    report_line(r, "PSO archive: %llu pipelines could not be added so far; first error: %s",
                (unsigned long long)atomic_load(&g_add_failed), g_add_error);
    g_add_error[0] = '\0';
  }
  if (!n)
    return 0;
  uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  uint64_t size = 0;
  char why[1024];
  const char *failed = pso_write_archive(g_current, &size, why, sizeof(why));
  const char *retried = "";
  char retry_note[2048] = "";
  if (failed) {
    /* gw2 (#52): say which pipeline Metal named, then rebuild without it and try once more */
    long idx = pso_failed_index(failed);
    char shape[512] = "not a recorded pipeline";
    if (idx >= 0 && (uint64_t)idx < g_rec_count && !g_rec[idx].skipped) {
      pso_key(g_rec[idx].desc, g_rec[idx].compute, shape, sizeof(shape));
      g_rec[idx].skipped = true;
      atomic_fetch_add(&g_skipped, 1);
    }
    report_line(r, "PSO archive: flush of %s failed: %s", g_path.lastPathComponent.UTF8String, failed);
    if (idx >= 0)
      report_line(r, "PSO archive: Metal names pipeline no. %ld; this session's pipeline no. %ld is: %s (left out "
                  "of the archive from now on)",
                  idx, idx, shape);
    uint64_t readded = 0;
    id<MTLBinaryArchive> fresh = g_device ? pso_rebuild(g_device, &readded) : nil;
    char why2[1024];
    const char *failed2 = fresh ? pso_write_archive(fresh, &size, why2, sizeof(why2)) : "cannot create an archive";
    if (!failed2) {
      [g_current release];
      g_current = fresh;
      atomic_fetch_add(&g_rebuilds, 1);
      snprintf(retry_note, sizeof(retry_note), " (rebuilt with %llu pipelines after a failed flush)",
               (unsigned long long)readded);
      retried = retry_note;
      failed = NULL;
    } else {
      [fresh release];
      snprintf(retry_note, sizeof(retry_note), "%s; the rebuilt archive failed too: %s", failed, failed2);
      failed = retry_note;
    }
  }
  double ms = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
  if (failed) {
    atomic_fetch_add(&g_unflushed, n);
    report_line(r, "PSO archive: flush of %s failed after %.0f ms: %s", g_path.lastPathComponent.UTF8String, ms,
                failed);
    return -1;
  }
  pso_write_keys(g_path);
  atomic_fetch_add(&g_flushes, 1);
  atomic_store(&g_flushed_bytes, size);
  atomic_store(&g_persisted, added);
  report_line(r, "PSO archive: flushed %s: %llu pipelines (+%llu), %.1f MB in %.0f ms%s%s",
              g_path.lastPathComponent.UTF8String, (unsigned long long)atomic_load(&g_added),
              (unsigned long long)n, mib(size), ms,
              atomic_load(&g_capped) ? " (session cap reached, later pipelines not added)" : "", retried);
  return 1;
}

static void
pso_report_atexit(void) {
  /* Exit does no Metal or disk work: serializing can take long, and the threads Metal depends on
   * may already be gone. The periodic flushes bound what is lost (at most DXMT_PSO_FLUSH_EVERY - 1
   * pipelines or 30 s); only report it. Counted against the last completed flush (one still
   * running counts as not done); an add in progress can count twice, hence "about". */
  uint64_t persisted = atomic_load(&g_persisted); /* first: never above the g_added read after it */
  uint64_t n = atomic_load(&g_added) - persisted + atomic_load(&g_queued);
  if (n)
    fprintf(stderr,
            "dxmt: PSO archive: about %llu pipelines added since the last flush are not saved (no flush at exit)\n",
            (unsigned long long)n);
}

static void
pso_queue_add(id desc, bool compute, uint64_t key) {
  if (atomic_fetch_add(&g_reserved, 1) >= g_max_pipelines) {
    atomic_fetch_add(&g_capped, 1);
    return;
  }
  atomic_fetch_add(&g_queued, 1);
  [desc retain];
  dispatch_async(g_queue, ^{
    @autoreleasepool {
      NSError *err = nil;
      BOOL ok;
      pthread_mutex_lock(&g_lock);
      if (compute)
        ok = [g_current addComputePipelineFunctionsWithDescriptor:desc error:&err];
      else
        ok = [g_current addRenderPipelineFunctionsWithDescriptor:desc error:&err];
      if (ok) {
        atomic_fetch_add(&g_added, 1);
        atomic_fetch_add(&g_unflushed, 1);
        /* gw2 (#52): record it for rebuilds and the .keys sidecar */
        if (g_rec_count == g_rec_cap) {
          uint64_t cap = g_rec_cap ? g_rec_cap * 2 : 256;
          struct pso_record *grown = realloc(g_rec, cap * sizeof(*g_rec));
          if (grown) {
            g_rec = grown;
            g_rec_cap = cap;
          }
        }
        if (g_rec_count < g_rec_cap) {
          g_rec[g_rec_count].key = key;
          g_rec[g_rec_count].desc = [desc retain];
          g_rec[g_rec_count].compute = compute;
          g_rec[g_rec_count].skipped = false;
          g_rec_count++;
        }
      } else {
        atomic_fetch_add(&g_add_failed, 1);
        if (!g_add_error[0])
          snprintf(g_add_error, sizeof(g_add_error), "%s",
                   err ? err.localizedDescription.UTF8String : "(no error)");
      }
      pthread_mutex_unlock(&g_lock);
    }
    [desc release];
    atomic_fetch_sub(&g_queued, 1);
  });
}

id<MTLRenderPipelineState>
pso_archive_new_render(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **err) {
  id<MTLRenderPipelineState> pso = nil;
  if (g_lookup.count) {
    desc.binaryArchives = g_lookup;
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
    atomic_fetch_add(&g_hits, 1);
  } else {
    atomic_fetch_add(&g_misses, 1);
    atomic_fetch_add(pso_known_has(key) ? &g_miss_known : &g_miss_new, 1);
    pso = [device newRenderPipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:err];
  }
  if (pso)
    pso_queue_add(desc, false, key);
  return pso;
}

id<MTLComputePipelineState>
pso_archive_new_compute(id<MTLDevice> device, MTLComputePipelineDescriptor *desc, NSError **err) {
  id<MTLComputePipelineState> pso = nil;
  if (g_lookup.count) {
    desc.binaryArchives = g_lookup;
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
    atomic_fetch_add(&g_hits, 1);
  } else {
    atomic_fetch_add(&g_misses, 1);
    atomic_fetch_add(pso_known_has(key) ? &g_miss_known : &g_miss_new, 1);
    pso = [device newComputePipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:err];
  }
  if (pso)
    pso_queue_add(desc, true, key);
  return pso;
}

static bool
pso_archive_init(id<MTLDevice> device, const struct WMTPSOArchiveConfig *cfg, struct report *r) {
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

  /* Collect archives; drop leftovers of interrupted writes and orphaned .sum files. */
  NSMutableArray<NSString *> *names = [NSMutableArray array];
  time_t now = time(NULL);
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
  [names sortUsingComparator:^NSComparisonResult(NSString *a, NSString *b) { return [b compare:a]; }];

  NSMutableArray *archives = [NSMutableArray array];
  uint64_t loaded_bytes = 0;
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
    id<MTLBinaryArchive> a = [device newBinaryArchiveWithDescriptor:ad error:&err];
    [ad release];
    if (!a) {
      pso_remove_archive(path);
      report_line(r, "PSO archive: deleted %s: Metal refused it: %s", name.UTF8String,
                  err ? err.localizedDescription.UTF8String : "(no error)");
      continue;
    }
    [archives addObject:a];
    [a release];
    loaded_bytes += size;
    keyed_archives += pso_read_keys(path, &keys, &key_count) > 0;
    report_line(r, "PSO archive: loaded %s (%.1f MB)", name.UTF8String, mib(size));
  }

  MTLBinaryArchiveDescriptor *cd = [[MTLBinaryArchiveDescriptor alloc] init];
  NSError *err = nil;
  g_current = [device newBinaryArchiveWithDescriptor:cd error:&err];
  [cd release];
  if (!g_current) {
    report_line(r, "PSO archive: off: cannot create this session's archive: %s",
                err ? err.localizedDescription.UTF8String : "(no error)");
    return false;
  }
  g_lookup = [archives copy];
  g_loaded = archives.count;
  /* gw2 (#52): the loaded sessions' keys, for classifying misses */
  if (key_count) {
    uint64_t cap = 64;
    while (cap < key_count * 2)
      cap <<= 1;
    g_known = calloc(cap, sizeof(uint64_t));
    if (g_known) {
      g_known_mask = cap - 1;
      for (uint64_t i = 0; i < key_count; i++)
        pso_known_insert(keys[i] ? keys[i] : 1);
    }
  }
  free(keys);
  g_device = [device retain];
  g_max_pipelines = cfg->max_pipelines;
  g_path = [[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%010llu-%d.mtlarchive",
                                                                           (unsigned long long)now, (int)getpid()]]
      retain];
  g_queue = dispatch_queue_create("dxmt.pso-archive",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
  atexit(pso_report_atexit);
  atomic_store_explicit(&g_enabled, true, memory_order_release);
  report_line(r, "PSO archive: on: %lu archives for lookup (%.1f MB) in %s; this session writes %s (keep %u sessions, "
              "at most %u pipelines each)",
              (unsigned long)archives.count, mib(loaded_bytes), dir.UTF8String, g_path.lastPathComponent.UTF8String,
              cfg->keep_sessions, cfg->max_pipelines);
  report_line(r, "PSO archive: %llu pipeline keys from %llu of the loaded archives classify lookup misses (known/new)",
              (unsigned long long)key_count, (unsigned long long)keyed_archives);
  return true;
}

int
_WMTPSOArchiveInit(void *obj) {
  struct unixcall_wmt_pso_archive_init *params = obj;
  static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
  static bool initialized;
  struct report r = {params->report.ptr, params->report_size, 0};
  if (r.buf && r.size)
    r.buf[0] = '\0';
  pthread_mutex_lock(&init_lock);
  if (initialized) {
    params->ret = pso_archive_enabled();
  } else {
    initialized = true;
    @autoreleasepool {
      params->ret = pso_archive_init((id<MTLDevice>)params->device, params->config.ptr, &r);
    }
  }
  pthread_mutex_unlock(&init_lock);
  return 0;
}

int
_WMTPSOArchiveFlush(void *obj) {
  struct unixcall_wmt_pso_archive_flush *params = obj;
  struct report r = {params->report.ptr, params->report_size, 0};
  struct report *rp = &r;
  if (r.buf && r.size)
    r.buf[0] = '\0';
  if (!pso_archive_enabled()) {
    params->ret = -1;
    return 0;
  }
  __block int ret = 0;
  /* Behind every add queued so far; only the calling thread waits. */
  dispatch_sync(g_queue, ^{
    @autoreleasepool {
      pthread_mutex_lock(&g_lock);
      ret = pso_flush_locked(rp);
      pthread_mutex_unlock(&g_lock);
    }
  });
  params->ret = ret;
  return 0;
}

int
_WMTPSOArchiveQuery(void *obj) {
  struct unixcall_wmt_pso_archive_query *params = obj;
  struct WMTPSOArchiveStats *s = params->stats.ptr;
  if (!s)
    return 0;
  s->loaded = pso_archive_enabled() ? g_loaded : 0;
  s->lookup_hits = atomic_load(&g_hits);
  s->lookup_misses = atomic_load(&g_misses);
  s->added = atomic_load(&g_added);
  s->add_failed = atomic_load(&g_add_failed);
  s->capped = atomic_load(&g_capped);
  s->queued = atomic_load(&g_queued);
  s->unflushed = atomic_load(&g_unflushed);
  s->flushes = atomic_load(&g_flushes);
  s->flushed_bytes = atomic_load(&g_flushed_bytes);
  s->miss_known = atomic_load(&g_miss_known); /* gw2 (#52) */
  s->miss_new = atomic_load(&g_miss_new);
  s->skipped = atomic_load(&g_skipped);
  s->rebuilds = atomic_load(&g_rebuilds);
  return 0;
}
