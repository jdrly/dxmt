#pragma once

// gw2 (#56): MetalFX for GW2's Subsample 3D pass (DXMT_SUBSAMPLE_METALFX), logged as the SUBSAMPLE
// section (see dxmt_gw2_stats.hpp).
//
// With Render Sampling: Subsample, GW2 renders the 3D scene below the backbuffer's size, then a
// full-screen draw samples it into a target of the swapchain's size; the UI is drawn on top after it.
// The front end (D3D11 Draw and DrawIndexed) recognises that draw (a candidate: a full-screen draw
// whose pixel shader reads a 2D SRV smaller than the target by a fixed ratio) and, with =1, has
// MetalFX spatial upscale that SRV into a private texture of the target's size, which the draw then
// samples in place of the game's SRV. The game's own pixel shader still runs: only its input is
// sharper. =log only logs the candidates' shapes.

#include <cstddef>
#include <cstdint>
#include <string>

namespace dxmt::gw2 {

enum class SubsampleMode { Off, Log, On };

// DXMT_SUBSAMPLE_METALFX: unset or 0 off, `log` capture only, `1` upscale. Resolved and logged once.
SubsampleMode subsampleMode();
// "0", "log" or "1", for the startup line
const char *subsampleModeName();
// DXMT_SUBSAMPLE_METALFX_PS: a lower-case hex prefix of the pixel shader's SHA-1 the pass must have
// (empty: any)
const std::string &subsamplePsPin();

// Instruction counts of a pixel shader's DXBC (SHDR/SHEX chunk), taken at creation while the mode
// isn't Off. `load` is ld and ld_ms: a texel fetch at integer coordinates, which would read the wrong
// texels from a larger texture.
struct DxbcCounts {
  bool parsed = false;
  uint32_t sample = 0;  // sample, sample_c, sample_c_lz, sample_l, sample_d, sample_b, gather4*
  uint32_t load = 0;    // ld, ld_ms
  uint32_t resinfo = 0; // resinfo
  uint32_t total = 0;   // instructions, declarations included
};
DxbcCounts scanDxbc(const void *bytecode, size_t length);
// keyed by the shader's SHA-1 (hex), as the pipeline cache names it
void notePixelShader(const std::string &sha1, const void *bytecode, size_t length);
DxbcCounts pixelShaderCounts(const std::string &sha1);

// The size of the swapchain's buffers, set at each ResizeBuffers (0x0 before the first)
void noteSwapchainSize(uint32_t width, uint32_t height);
void swapchainSize(uint32_t &width, uint32_t &height);

// True the first time this candidate key is seen (at most 64 keys, then false with one line saying so)
bool subsampleFirstSighting(uint64_t key);

// SUBSAMPLE's window counts (the render thread, relaxed atomics)
void subsampleCandidate(bool accepted);
void subsampleUpscaled();

} // namespace dxmt::gw2
