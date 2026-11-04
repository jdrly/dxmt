/*
 * gw2 (B2, jdrly/dxmt #67): the bridge's call manifest. Every bridge call's number is written here
 * once; the PE side's enum WMTCall (winemetal_call_numbers.h) and the unix side's two tables
 * (__wine_unix_call_funcs and __wine_unix_call_wow64_funcs, winemetal_unix.c) are generated from it.
 *
 *   WMT_CALL(number, name, handler)                      the same unix handler in both tables
 *   WMT_CALL_WOW64(number, name, handler, wow64_handler)  the wow64 table needs its own handler
 *   WMT_RESERVED(number, "who reserved it")             a hole: NULL in both tables
 *
 * The rules (ADR 0005, docs/gw2/adr/0005-bridge-call-numbers-in-one-manifest.md):
 * - One line per call number, in order, from 0 with no gap: the build rejects a duplicate, a gap or
 *   a line out of order (the checks in winemetal_call_numbers.h).
 * - Code names a call, never its number: UNIX_CALL(NSObject_retain, ...) in the thunks (B3, #76).
 * - Append only. A new call takes the next number, or turns its own WMT_RESERVED line into a
 *   WMT_CALL. Never renumber a call; PE and unix ship together, so compaction is allowed but is a
 *   deliberate change of its own.
 * - A hole is WMT_RESERVED with the issue (or lane) that reserved it.
 *
 * No include guard: an includer defines the three macros, includes this file, and gets them back
 * undefined.
 */

/* clang-format off */

/* upstream DXMT, 0-146 */
WMT_CALL(0, NSObject_retain, _NSObject_retain)
WMT_CALL(1, NSObject_release, _NSObject_release)
WMT_CALL(2, NSArray_object, _NSArray_object)
WMT_CALL(3, NSArray_count, _NSArray_count)
WMT_CALL(4, MTLCopyAllDevices, _MTLCopyAllDevices)
WMT_CALL(5, MTLDevice_recommendedMaxWorkingSetSize, _MTLDevice_recommendedMaxWorkingSetSize)
WMT_CALL(6, MTLDevice_currentAllocatedSize, _MTLDevice_currentAllocatedSize)
WMT_CALL(7, MTLDevice_name, _MTLDevice_name)
WMT_CALL(8, NSString_getCString, _NSString_getCString)
WMT_CALL(9, MTLDevice_newCommandQueue, _MTLDevice_newCommandQueue)
WMT_CALL(10, NSAutoreleasePool_alloc_init, _NSAutoreleasePool_alloc_init)
WMT_CALL(11, MTLCommandQueue_commandBuffer, _MTLCommandQueue_commandBuffer)
WMT_CALL(12, MTLCommandBuffer_commit, _MTLCommandBuffer_commit)
WMT_CALL(13, MTLCommandBuffer_waitUntilCompleted, _MTLCommandBuffer_waitUntilCompleted)
WMT_CALL(14, MTLCommandBuffer_status, _MTLCommandBuffer_status)
WMT_CALL(15, MTLDevice_newSharedEvent, _MTLDevice_newSharedEvent)
WMT_CALL(16, MTLSharedEvent_signaledValue, _MTLSharedEvent_signaledValue)
WMT_CALL(17, MTLCommandBuffer_encodeSignalEvent, _MTLCommandBuffer_encodeSignalEvent)
WMT_CALL(18, MTLDevice_newBuffer, _MTLDevice_newBuffer)
WMT_CALL(19, MTLDevice_newSamplerState, _MTLDevice_newSamplerState)
WMT_CALL(20, MTLDevice_newDepthStencilState, _MTLDevice_newDepthStencilState)
WMT_CALL(21, MTLDevice_newTexture, _MTLDevice_newTexture)
WMT_CALL(22, MTLBuffer_newTexture, _MTLBuffer_newTexture)
WMT_CALL(23, MTLTexture_newTextureView, _MTLTexture_newTextureView)
WMT_CALL(24, MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
         _MTLDevice_minimumLinearTextureAlignmentForPixelFormat)
WMT_CALL(25, MTLDevice_newLibrary, _MTLDevice_newLibrary)
WMT_CALL(26, MTLLibrary_newFunction, _MTLLibrary_newFunction)
WMT_CALL(27, NSString_lengthOfBytesUsingEncoding, _NSString_lengthOfBytesUsingEncoding)
WMT_CALL(28, NSObject_description, _NSObject_description)
WMT_CALL(29, MTLDevice_newComputePipelineState, _MTLDevice_newComputePipelineState)
WMT_CALL(30, MTLCommandBuffer_blitCommandEncoder, _MTLCommandBuffer_blitCommandEncoder)
WMT_CALL(31, MTLCommandBuffer_computeCommandEncoder, _MTLCommandBuffer_computeCommandEncoder)
WMT_CALL(32, MTLCommandBuffer_renderCommandEncoder, _MTLCommandBuffer_renderCommandEncoder)
WMT_CALL(33, MTLCommandEncoder_endEncoding, _MTLCommandEncoder_endEncoding)
WMT_CALL(34, MTLDevice_newRenderPipelineState, _MTLDevice_newRenderPipelineState)
WMT_CALL(35, MTLDevice_newMeshRenderPipelineState, _MTLDevice_newMeshRenderPipelineState)
WMT_CALL(36, MTLBlitCommandEncoder_encodeCommands, _MTLBlitCommandEncoder_encodeCommands)
WMT_CALL(37, MTLComputeCommandEncoder_encodeCommands, _MTLComputeCommandEncoder_encodeCommands)
WMT_CALL(38, MTLRenderCommandEncoder_encodeCommands, _MTLRenderCommandEncoder_encodeCommands)
WMT_CALL(39, MTLTexture_pixelFormat, _MTLTexture_pixelFormat)
WMT_CALL(40, MTLTexture_width, _MTLTexture_width)
WMT_CALL(41, MTLTexture_height, _MTLTexture_height)
WMT_CALL(42, MTLTexture_depth, _MTLTexture_depth)
WMT_CALL(43, MTLTexture_arrayLength, _MTLTexture_arrayLength)
WMT_CALL(44, MTLTexture_mipmapLevelCount, _MTLTexture_mipmapLevelCount)
WMT_CALL(45, MTLTexture_replaceRegion, _MTLTexture_replaceRegion)
WMT_CALL(46, MTLBuffer_didModifyRange, _MTLBuffer_didModifyRange)
WMT_CALL(47, MTLCommandBuffer_presentDrawable, _MTLCommandBuffer_presentDrawable)
WMT_CALL(48, MTLCommandBuffer_presentDrawableAfterMinimumDuration,
         _MTLCommandBuffer_presentDrawableAfterMinimumDuration)
WMT_CALL(49, MTLDevice_supportsFamily, _MTLDevice_supportsFamily)
WMT_CALL(50, MTLDevice_supportsBCTextureCompression, _MTLDevice_supportsBCTextureCompression)
WMT_CALL(51, MTLDevice_supportsTextureSampleCount, _MTLDevice_supportsTextureSampleCount)
WMT_CALL(52, MTLDevice_hasUnifiedMemory, _MTLDevice_hasUnifiedMemory)
WMT_CALL(53, MTLCaptureManager_sharedCaptureManager, _MTLCaptureManager_sharedCaptureManager)
WMT_CALL(54, MTLCaptureManager_startCapture, _MTLCaptureManager_startCapture)
WMT_CALL(55, MTLCaptureManager_stopCapture, _MTLCaptureManager_stopCapture)
WMT_CALL(56, MTLDevice_newTemporalScaler, _MTLDevice_newTemporalScaler)
WMT_CALL(57, MTLDevice_newSpatialScaler, _MTLDevice_newSpatialScaler)
WMT_CALL(58, MTLCommandBuffer_encodeTemporalScale, _MTLCommandBuffer_encodeTemporalScale)
WMT_CALL(59, MTLCommandBuffer_encodeSpatialScale, _MTLCommandBuffer_encodeSpatialScale)
WMT_CALL(60, NSString_string, _NSString_string)
WMT_CALL(61, NSString_alloc_init, _NSString_alloc_init)
WMT_CALL(62, DeveloperHUDProperties_instance, _DeveloperHUDProperties_instance)
WMT_CALL(63, DeveloperHUDProperties_addLabel, _DeveloperHUDProperties_addLabel)
WMT_CALL(64, DeveloperHUDProperties_updateLabel, _DeveloperHUDProperties_updateLabel)
WMT_CALL(65, DeveloperHUDProperties_remove, _DeveloperHUDProperties_remove)
WMT_CALL(66, MetalDrawable_texture, _MetalDrawable_texture)
WMT_CALL(67, MetalLayer_nextDrawable, _MetalLayer_nextDrawable)
WMT_CALL(68, MTLDevice_supportsFXSpatialScaler, _MTLDevice_supportsFXSpatialScaler)
WMT_CALL(69, MTLDevice_supportsFXTemporalScaler, _MTLDevice_supportsFXTemporalScaler)
WMT_CALL(70, MetalLayer_setProps, _MetalLayer_setProps)
WMT_CALL(71, MetalLayer_getProps, _MetalLayer_getProps)
WMT_CALL(72, CreateMetalViewFromHWND, _CreateMetalViewFromHWND)
WMT_CALL(73, ReleaseMetalView, _ReleaseMetalView)
WMT_CALL_WOW64(74, SM50Initialize, thunk_SM50Initialize, thunk32_SM50Initialize)
WMT_CALL(75, SM50Destroy, thunk_SM50Destroy)
WMT_CALL_WOW64(76, SM50Compile, thunk_SM50Compile, thunk32_SM50Compile)
WMT_CALL_WOW64(77, SM50GetCompiledBitcode, thunk_SM50GetCompiledBitcode, thunk32_SM50GetCompiledBitcode)
WMT_CALL(78, SM50DestroyBitcode, thunk_SM50DestroyBitcode)
WMT_CALL_WOW64(79, SM50GetErrorMessage, thunk_SM50GetErrorMessage, thunk32_SM50GetErrorMessage)
WMT_CALL(80, SM50FreeError, thunk_SM50FreeError)
WMT_CALL_WOW64(81, SM50CompileGeometryPipelineVertex,
         thunk_SM50CompileGeometryPipelineVertex, thunk32_SM50CompileGeometryPipelineVertex)
WMT_CALL_WOW64(82, SM50CompileGeometryPipelineGeometry,
         thunk_SM50CompileGeometryPipelineGeometry, thunk32_SM50CompileGeometryPipelineGeometry)
WMT_RESERVED(83, "upstream: unix_sm50_compile_tessellation_vertex (airconv_thunks.h), never wired")
WMT_CALL_WOW64(84, SM50CompileTessellationPipelineHull,
         thunk_SM50CompileTessellationPipelineHull, thunk32_SM50CompileTessellationPipelineHull)
WMT_CALL_WOW64(85, SM50CompileTessellationPipelineDomain,
         thunk_SM50CompileTessellationPipelineDomain, thunk32_SM50CompileTessellationPipelineDomain)
WMT_CALL(86, MTLCommandEncoder_setLabel, _MTLCommandEncoder_setLabel)
WMT_CALL(87, MTLDevice_setShouldMaximizeConcurrentCompilation,
         _MTLDevice_setShouldMaximizeConcurrentCompilation)
WMT_CALL_WOW64(88, SM50GetArgumentsInfo, thunk_SM50GetArgumentsInfo, thunk32_SM50GetArgumentsInfo)
WMT_CALL(89, MTLCommandBuffer_error, _MTLCommandBuffer_error)
WMT_CALL(90, MTLCommandBuffer_logs, _MTLCommandBuffer_logs)
WMT_CALL(91, MTLLogContainer_enumerate, _MTLLogContainer_enumerate)
WMT_CALL(92, CGColorSpace_checkColorSpaceSupported, _CGColorSpace_checkColorSpaceSupported)
WMT_CALL(93, MetalLayer_setColorSpace, _MetalLayer_setColorSpace)
WMT_CALL(94, WMTGetPrimaryDisplayId, _WMTGetPrimaryDisplayId)
WMT_CALL(95, WMTGetSecondaryDisplayId, _WMTGetSecondaryDisplayId)
WMT_CALL(96, WMTGetDisplayDescription, _WMTGetDisplayDescription)
WMT_CALL(97, MetalLayer_getEDRValue, _MetalLayer_getEDRValue)
WMT_CALL(98, MTLLibrary_newFunctionWithConstants, _MTLLibrary_newFunctionWithConstants)
WMT_CALL(99, WMTQueryDisplaySetting, _WMTQueryDisplaySetting)
WMT_CALL(100, WMTUpdateDisplaySetting, _WMTUpdateDisplaySetting)
WMT_CALL(101, WMTQueryDisplaySettingForLayer, _WMTQueryDisplaySettingForLayer)
WMT_CALL(102, MTLCommandBuffer_encodeWaitForEvent, _MTLCommandBuffer_encodeWaitForEvent)
WMT_CALL(103, MTLSharedEvent_signalValue, _MTLSharedEvent_signalValue)
WMT_CALL(104, MTLSharedEvent_setWin32EventAtValue, _MTLSharedEvent_setWin32EventAtValue)
WMT_CALL(105, MTLDevice_newFence, _MTLDevice_newFence)
WMT_CALL(106, MTLDevice_newEvent, _MTLDevice_newEvent)
WMT_CALL(107, MTLBuffer_updateContents, _MTLBuffer_updateContents)
WMT_CALL(108, SharedEventListener_create, _SharedEventListener_create)
WMT_CALL(109, SharedEventListener_start, _SharedEventListener_start)
WMT_CALL(110, SharedEventListener_destroy, _SharedEventListener_destroy)
WMT_CALL(111, WMTGetOSVersion, _WMTGetOSVersion)
WMT_CALL(112, MTLDevice_newBinaryArchive, _MTLDevice_newBinaryArchive)
WMT_CALL(113, MTLBinaryArchive_serialize, _MTLBinaryArchive_serialize)
WMT_CALL(114, DispatchData_alloc_init, _DispatchData_alloc_init)
WMT_CALL(115, CacheReader_alloc_init, _CacheReader_alloc_init)
WMT_CALL(116, CacheReader_get, _CacheReader_get)
WMT_CALL(117, CacheWriter_alloc_init, _CacheWriter_alloc_init)
WMT_CALL(118, CacheWriter_set, _CacheWriter_set)
WMT_CALL(119, WMTSetMetalShaderCachePath, _WMTSetMetalShaderCachePath)
WMT_CALL(120, MTLDevice_newSharedTexture, _MTLDevice_newSharedTexture)
WMT_CALL(121, WMTBootstrapRegister, _WMTBootstrapRegister)
WMT_CALL(122, WMTBootstrapLookUp, _WMTBootstrapLookUp)
WMT_CALL(123, MTLSharedEvent_createMachPort, _MTLSharedEvent_createMachPort)
WMT_CALL(124, MTLDevice_newSharedEventWithMachPort, _MTLDevice_newSharedEventWithMachPort)
WMT_CALL(125, MTLDevice_registryID, _MTLDevice_registryID)
WMT_CALL(126, MTLSharedEvent_waitUntilSignaledValue, _MTLSharedEvent_waitUntilSignaledValue)
WMT_CALL(127, MTLCounterSampleBuffer_newTimestampBuffer, _MTLCounterSampleBuffer_newTimestampBuffer)
WMT_CALL(128, MTLCounterSampleBuffer_resolveCounterRange, _MTLCounterSampleBuffer_resolveCounterRange)
WMT_CALL(129, MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
         _MTLCommandBuffer_blitCommandEncoderWithSampleBuffers)
WMT_CALL(130, MTLCommandBuffer_property, _MTLCommandBuffer_property)
WMT_CALL(131, MTLDevice_newTileRenderPipelineState, _MTLDevice_newTileRenderPipelineState)
WMT_CALL(132, MTLDevice_newResidencySet, _MTLDevice_newResidencySet)
WMT_CALL(133, MTLResidencySet_addAllocations, _MTLResidencySet_addAllocations)
WMT_CALL(134, MTLResidencySet_removeAllocations, _MTLResidencySet_removeAllocations)
WMT_CALL(135, MTLResidencySet_removeAllAllocations, _MTLResidencySet_removeAllAllocations)
WMT_CALL(136, MTLResidencySet_commit, _MTLResidencySet_commit)
WMT_CALL(137, MTLCommandQueue_addResidencySet, _MTLCommandQueue_addResidencySet)
WMT_CALL(138, MTLDevice_newHeap, _MTLDevice_newHeap)
WMT_CALL(139, MTLDevice_heapBufferSizeAndAlign, _MTLDevice_heapBufferSizeAndAlign)
WMT_CALL(140, MTLDevice_heapTextureSizeAndAlign, _MTLDevice_heapTextureSizeAndAlign)
WMT_CALL(141, MTLHeap_newBuffer, _MTLHeap_newBuffer)
WMT_CALL(142, MTLHeap_newTexture, _MTLHeap_newTexture)
WMT_CALL(143, MTLDevice_newIndirectCommandBuffer, _MTLDevice_newIndirectCommandBuffer)
WMT_CALL(144, MTLDevice_newLibraryWithSource, _MTLDevice_newLibraryWithSource)
WMT_CALL(145, MTLTexture_getBytes, _MTLTexture_getBytes)
WMT_CALL(146, MTLCommandBuffer_presentDrawableWithDeadline, _MTLCommandBuffer_presentDrawableWithDeadline)
/* gw2 (#1) */
WMT_CALL(147, MetalView_visibility, _MetalView_visibility)
/* gw2 (#7) */
WMT_CALL(148, MTLDrawable_trackPresented, _MTLDrawable_trackPresented)
/* gw2 (#5): PSO archives, pso_archive.c */
WMT_CALL(149, WMTPSOArchiveInit, _WMTPSOArchiveInit)
WMT_CALL(150, WMTPSOArchiveFlush, _WMTPSOArchiveFlush)
WMT_CALL(151, WMTPSOArchiveQuery, _WMTPSOArchiveQuery)
/* gw2 (#46, under #5): cache.c */
WMT_CALL(152, WMTGetMetalShaderCachePath, _WMTGetMetalShaderCachePath)
/* gw2 (#9) */
WMT_CALL(153, WMTSetThreadQoSClassSelf, _WMTSetThreadQoSClassSelf)
/* gw2 (#31) */
WMT_CALL(154, MTLCommandQueue_commandBufferWithUnretainedReferences,
         _MTLCommandQueue_commandBufferWithUnretainedReferences)
/* gw2 (#32): Metal 4, mtl4.c */
WMT_CALL(155, MTLDevice_metal4Support, _MTLDevice_metal4Support)
WMT_CALL(156, MTLDevice_newMTL4CommandQueue, _MTLDevice_newMTL4CommandQueue)
WMT_CALL(157, MTLDevice_newCommandAllocator, _MTLDevice_newCommandAllocator)
WMT_CALL(158, MTLDevice_newMTL4CommandBuffer, _MTLDevice_newMTL4CommandBuffer)
WMT_CALL(159, MTLDevice_newArgumentTable, _MTLDevice_newArgumentTable)
WMT_CALL(160, MTL4CommandAllocator_reset, _MTL4CommandAllocator_reset)
WMT_CALL(161, MTL4CommandBuffer_beginCommandBuffer, _MTL4CommandBuffer_beginCommandBuffer)
WMT_CALL(162, MTL4CommandBuffer_endCommandBuffer, _MTL4CommandBuffer_endCommandBuffer)
WMT_CALL(163, MTL4CommandBuffer_useResidencySet, _MTL4CommandBuffer_useResidencySet)
WMT_CALL(164, MTL4CommandQueue_commit, _MTL4CommandQueue_commit)
WMT_CALL(165, MTL4CommandQueue_waitForDrawable, _MTL4CommandQueue_waitForDrawable)
WMT_CALL(166, MTL4CommandQueue_signalDrawable, _MTL4CommandQueue_signalDrawable)
WMT_CALL(167, MTL4CommandQueue_signalEvent, _MTL4CommandQueue_signalEvent)
WMT_CALL(168, MTL4CommandQueue_waitForEvent, _MTL4CommandQueue_waitForEvent)
WMT_CALL(169, MTL4CommandQueue_addResidencySet, _MTL4CommandQueue_addResidencySet)
WMT_CALL(170, MTL4CommandQueue_removeResidencySet, _MTL4CommandQueue_removeResidencySet)
WMT_CALL(171, MetalLayer_residencySet, _MetalLayer_residencySet)
WMT_CALL(172, MetalDrawable_present, _MetalDrawable_present)
WMT_CALL(173, MTL4CommandBuffer_renderCommandEncoder, _MTL4CommandBuffer_renderCommandEncoder)
WMT_CALL(174, MTL4CommandBuffer_computeCommandEncoder, _MTL4CommandBuffer_computeCommandEncoder)
WMT_CALL(175, MTL4RenderCommandEncoder_encodeCommands, _MTL4RenderCommandEncoder_encodeCommands)
WMT_CALL(176, MTL4ComputeCommandEncoder_encodeCommands, _MTL4ComputeCommandEncoder_encodeCommands)
WMT_CALL(177, MTL4ComputeCommandEncoder_encodeBlitCommands, _MTL4ComputeCommandEncoder_encodeBlitCommands)
WMT_CALL(178, MTL4CommandEncoder_barrier, _MTL4CommandEncoder_barrier)
WMT_RESERVED(179, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(180, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(181, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(182, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(183, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(184, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(185, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(186, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(187, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(188, "jdrly/dxmt #32 (parts 2-4)")
WMT_RESERVED(189, "jdrly/dxmt #32 (parts 2-4)")
/* gw2 (#56): Metal 4 MetalFX, mtl4fx.c */
WMT_CALL(190, MTLDevice_supportsMetal4FX, _MTLDevice_supportsMetal4FX)
WMT_CALL(191, MTLDevice_newGW2SpatialScaler, _MTLDevice_newGW2SpatialScaler)
WMT_CALL(192, MTL4CommandBuffer_encodeSpatialScale, _MTL4CommandBuffer_encodeSpatialScale)
/* reserved for lane C: shader warming and the compile service (jdrly/dxmt #64) */
WMT_CALL(193, CacheEnumerate, _CacheEnumerate) /* gw2 (W2/W3): the recipes, in batches */
WMT_RESERVED(194, "lane C, jdrly/dxmt #64")
WMT_RESERVED(195, "lane C, jdrly/dxmt #64")
WMT_RESERVED(196, "lane C, jdrly/dxmt #64")
WMT_RESERVED(197, "lane C, jdrly/dxmt #64")
WMT_RESERVED(198, "lane C, jdrly/dxmt #64")
WMT_RESERVED(199, "lane C, jdrly/dxmt #64")

/* clang-format on */

#undef WMT_CALL
#undef WMT_CALL_WOW64
#undef WMT_RESERVED
