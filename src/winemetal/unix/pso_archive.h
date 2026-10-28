#pragma once

/* gw2 (#5): persistent Metal binary archives for pipelines (DXMT_PSO_ARCHIVE=1). See
   pso_archive.c. */

#import <Metal/Metal.h>
#include <stdbool.h>

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
