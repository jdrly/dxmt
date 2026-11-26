/* gw2 (#32): the Metal 4 bridge calls (their numbers: winemetal_calls.h), in mtl4.c. A file
   of its own so that tests/gw2/mtl4_bridge_test.m can drive the same code natively. */
#ifndef __WINEMETAL_UNIX_MTL4_H
#define __WINEMETAL_UNIX_MTL4_H

typedef int NTSTATUS;

NTSTATUS _MTLDevice_metal4Support(void *obj);
NTSTATUS _MTLDevice_newMTL4CommandQueue(void *obj);
NTSTATUS _MTLDevice_newCommandAllocator(void *obj);
NTSTATUS _MTLDevice_newMTL4CommandBuffer(void *obj);
NTSTATUS _MTLDevice_newArgumentTable(void *obj);
NTSTATUS _MTL4CommandAllocator_reset(void *obj);
NTSTATUS _MTL4CommandBuffer_beginCommandBuffer(void *obj);
NTSTATUS _MTL4CommandBuffer_endCommandBuffer(void *obj);
NTSTATUS _MTL4CommandBuffer_useResidencySet(void *obj);
NTSTATUS _MTL4CommandQueue_commit(void *obj);
NTSTATUS _MTL4CommandQueue_commitPresent(void *obj); /* gw2 (#242) */
NTSTATUS _MTL4CommandQueue_waitForDrawable(void *obj);
NTSTATUS _MTL4CommandQueue_signalDrawable(void *obj);
NTSTATUS _MTL4CommandQueue_signalEvent(void *obj);
NTSTATUS _MTL4CommandQueue_waitForEvent(void *obj);
NTSTATUS _MTL4CommandQueue_addResidencySet(void *obj);
NTSTATUS _MTL4CommandQueue_removeResidencySet(void *obj);
NTSTATUS _MetalLayer_residencySet(void *obj);
NTSTATUS _MetalDrawable_present(void *obj);
NTSTATUS _MTL4CommandBuffer_renderCommandEncoder(void *obj);
NTSTATUS _MTL4CommandBuffer_computeCommandEncoder(void *obj);
NTSTATUS _MTL4RenderCommandEncoder_encodeCommands(void *obj);
NTSTATUS _MTL4ComputeCommandEncoder_encodeCommands(void *obj);
NTSTATUS _MTL4ComputeCommandEncoder_encodeBlitCommands(void *obj);
NTSTATUS _MTL4CommandEncoder_barrier(void *obj);

#endif
