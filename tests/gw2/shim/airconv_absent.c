// gw2 (B3, jdrly/dxmt #76): airconv's C API (src/airconv/airconv_public.h) for native tests that
// link the bridge's real unix side (every source in src/winemetal/unix/meson.build's
// winemetal_unix_src) without the shader compiler. winemetal_unix.c's SM50 handlers call these; the
// compiler itself is LLVM-sized and not under test, so each call here fails and says so. A test that
// compiles shaders links the real airconv instead (meson's bridge-native-smoke does).
#include <stdio.h>
#include "airconv_public.h"

static void
absent(const char *function) {
  fprintf(stderr, "airconv_absent.c: %s called, but this test has no shader compiler\n", function);
}

AIRCONV_API int
SM50Initialize(const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
               struct MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError) {
  (void)pBytecode, (void)BytecodeSize, (void)ppShader, (void)pRefl;
  absent(__func__);
  if (ppError)
    *ppError = NULL;
  return 1;
}

AIRCONV_API void
SM50Destroy(sm50_shader_t pShader) {
  (void)pShader;
  absent(__func__);
}

AIRCONV_API int
SM50Compile(sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName,
            sm50_bitcode_t *ppBitcode, sm50_error_t *ppError) {
  (void)pShader, (void)pArgs, (void)FunctionName, (void)ppBitcode;
  absent(__func__);
  if (ppError)
    *ppError = NULL;
  return 1;
}

AIRCONV_API void
SM50GetCompiledBitcode(sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData) {
  (void)pBitcode, (void)pData;
  absent(__func__);
}

AIRCONV_API void
SM50DestroyBitcode(sm50_bitcode_t pBitcode) {
  (void)pBitcode;
  absent(__func__);
}

AIRCONV_API size_t
SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) {
  (void)pError;
  absent(__func__);
  if (pBuffer && BufferSize)
    pBuffer[0] = '\0';
  return 0;
}

AIRCONV_API void
SM50FreeError(sm50_error_t pError) {
  (void)pError;
  absent(__func__);
}

AIRCONV_API int
SM50CompileTessellationPipelineHull(sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
                                    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
                                    const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError) {
  (void)pVertexShader, (void)pHullShader, (void)pHullShaderArgs, (void)FunctionName, (void)ppBitcode;
  absent(__func__);
  if (ppError)
    *ppError = NULL;
  return 1;
}

AIRCONV_API int
SM50CompileTessellationPipelineDomain(sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
                                      struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
                                      const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError) {
  (void)pHullShader, (void)pDomainShader, (void)pDomainShaderArgs, (void)FunctionName, (void)ppBitcode;
  absent(__func__);
  if (ppError)
    *ppError = NULL;
  return 1;
}

AIRCONV_API int
SM50CompileGeometryPipelineVertex(sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
                                  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
                                  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError) {
  (void)pVertexShader, (void)pGeometryShader, (void)pVertexShaderArgs, (void)FunctionName, (void)ppBitcode;
  absent(__func__);
  if (ppError)
    *ppError = NULL;
  return 1;
}

AIRCONV_API int
SM50CompileGeometryPipelineGeometry(sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
                                    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
                                    const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError) {
  (void)pVertexShader, (void)pGeometryShader, (void)pGeometryShaderArgs, (void)FunctionName, (void)ppBitcode;
  absent(__func__);
  if (ppError)
    *ppError = NULL;
  return 1;
}

AIRCONV_API void
SM50GetArgumentsInfo(sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
                     struct MTL_SM50_SHADER_ARGUMENT *pArguments) {
  (void)pShader, (void)pConstantBuffers, (void)pArguments;
  absent(__func__);
}
