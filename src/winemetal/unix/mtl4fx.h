/* gw2 (#56): MetalFX spatial upscaling of GW2's Subsample pass (bridge calls), in mtl4fx.c. A file of
   its own so that tests/gw2/mtl4fx_test.m can drive the same code natively. */
#ifndef __WINEMETAL_UNIX_MTL4FX_H
#define __WINEMETAL_UNIX_MTL4FX_H

typedef int NTSTATUS;

NTSTATUS _MTLDevice_supportsMetal4FX(void *obj);
NTSTATUS _MTLDevice_newGW2SpatialScaler(void *obj);
NTSTATUS _MTL4CommandBuffer_encodeSpatialScale(void *obj);

#endif
