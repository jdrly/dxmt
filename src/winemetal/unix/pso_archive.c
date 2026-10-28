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
 * own thread, WMTPSOArchiveFlush) and every 120 s if any are new. Exit never flushes; it only
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
  NSString *sum_path = [g_path stringByAppendingString:@".sum"];
  NSString *tmp = [g_path stringByAppendingString:@".tmp"];
  NSString *sum_tmp = [g_path stringByAppendingString:@".sum.tmp"];
  NSError *err = nil;
  const char *failed = NULL;
  uint64_t size = 0;
  if (![g_current serializeToURL:[NSURL fileURLWithPath:tmp] error:&err]) {
    failed = err ? err.localizedDescription.UTF8String : "serializeToURL failed";
  } else {
    NSString *sum = pso_sum_text(tmp, &size);
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
  double ms = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
  if (failed) {
    unlink(tmp.fileSystemRepresentation);
    unlink(sum_tmp.fileSystemRepresentation);
    atomic_fetch_add(&g_unflushed, n);
    report_line(r, "PSO archive: flush of %s failed after %.0f ms: %s", g_path.lastPathComponent.UTF8String, ms,
                failed);
    return -1;
  }
  atomic_fetch_add(&g_flushes, 1);
  atomic_store(&g_flushed_bytes, size);
  atomic_store(&g_persisted, added);
  report_line(r, "PSO archive: flushed %s: %llu pipelines (+%llu), %.1f MB in %.0f ms%s",
              g_path.lastPathComponent.UTF8String, (unsigned long long)atomic_load(&g_added),
              (unsigned long long)n, mib(size), ms,
              atomic_load(&g_capped) ? " (session cap reached, later pipelines not added)" : "");
  return 1;
}

static void
pso_report_atexit(void) {
  /* Exit does no Metal or disk work: serializing can take long, and the threads Metal depends on
   * may already be gone. The periodic flushes bound what is lost (at most DXMT_PSO_FLUSH_EVERY - 1
   * pipelines or 120 s); only report it. Counted against the last completed flush (one still
   * running counts as not done); an add in progress can count twice, hence "about". */
  uint64_t persisted = atomic_load(&g_persisted); /* first: never above the g_added read after it */
  uint64_t n = atomic_load(&g_added) - persisted + atomic_load(&g_queued);
  if (n)
    fprintf(stderr,
            "dxmt: PSO archive: about %llu pipelines added since the last flush are not saved (no flush at exit)\n",
            (unsigned long long)n);
}

static void
pso_queue_add(id desc, bool compute) {
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
  if (pso) {
    atomic_fetch_add(&g_hits, 1);
  } else {
    atomic_fetch_add(&g_misses, 1);
    pso = [device newRenderPipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:err];
  }
  if (pso)
    pso_queue_add(desc, false);
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
  if (pso) {
    atomic_fetch_add(&g_hits, 1);
  } else {
    atomic_fetch_add(&g_misses, 1);
    pso = [device newComputePipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:err];
  }
  if (pso)
    pso_queue_add(desc, true);
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
    } else if ([name hasSuffix:@".mtlarchive.sum"]) {
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
  return 0;
}
