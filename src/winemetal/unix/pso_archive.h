#pragma once

/* gw2 (#5): persistent Metal binary archives for pipelines (DXMT_PSO_ARCHIVE=1). See
   pso_archive.c. */

#import <Metal/Metal.h>
#include <stdbool.h>

struct WMTPSOArchiveConfig;
struct WMTPSOArchiveStats;
struct pso_report;

/* Set once by WMTPSOArchiveInit; false until then and when the archives are off. */
bool pso_archive_enabled(void);

/* Build a pipeline with the earlier sessions' archives for lookup, then queue it for this
   session's archive. The caller's descriptor must not set binaryArchives itself. */
id<MTLRenderPipelineState>
pso_archive_new_render(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **err);
id<MTLComputePipelineState>
pso_archive_new_compute(id<MTLDevice> device, MTLComputePipelineDescriptor *desc, NSError **err);

int _WMTPSOArchiveInit(void *obj);
int _WMTPSOArchiveFlush(void *obj);
int _WMTPSOArchiveQuery(void *obj);

/* gw2 (#75): the archive as an instance. The bridge calls above act on the process instance; the
   native archive tool (and the tests) can open their own, which live until exit. */
struct pso_archive;
struct pso_archive *pso_archive_process(void);
struct pso_archive *pso_archive_create(void);
/* Loads earlier sessions' archives (or the newest good generation) for lookup and starts capture;
   false (instance off) with the reason in `r`. Once per instance. */
bool pso_archive_open(struct pso_archive *a, id<MTLDevice> device, const struct WMTPSOArchiveConfig *cfg,
                      struct pso_report *r);
/* As WMTPSOArchiveFlush: 1 written, 0 nothing new, -1 failed or off. */
int pso_archive_flush(struct pso_archive *a, struct pso_report *r);
void pso_archive_query(struct pso_archive *a, struct WMTPSOArchiveStats *s);
id<MTLRenderPipelineState>
pso_archive_build_render(struct pso_archive *a, id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **err);
id<MTLComputePipelineState>
pso_archive_build_compute(struct pso_archive *a, id<MTLDevice> device, MTLComputePipelineDescriptor *desc,
                          NSError **err);
