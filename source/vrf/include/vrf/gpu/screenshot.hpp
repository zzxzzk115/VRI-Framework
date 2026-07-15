/*
 * screenshot.hpp - headless texture readback to host memory / PNG.
 *
 * Reads a rendered fg::Texture back to the CPU via a one-shot GPU readback
 * (CmdReadbackTextureToBuffer through a HostReadback staging buffer) - no
 * swapchain, window, or compositor is involved, so it works headless and on the
 * Metal backend where screen-capturing the CAMetalLayer of an inactive window
 * comes back black.
 *
 * Supports RGBA16_SFLOAT (half decoded to 8-bit) and 8-bit RGBA8/BGRA8 sources;
 * the result is tightly-packed 8-bit RGBA, top-left origin, with opaque alpha.
 * The source texture must be created with VriTextureUsage_TransferSrc (Vulkan/GL/
 * D3D12 require it; Metal tolerates any). Synchronous - submits and waits - so it
 * is meant for verification / CI, not per-frame use.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;
    namespace fg
    {
        class Texture;
    }

    // Tightly-packed 8-bit RGBA image read back from the GPU (top-left origin).
    struct HostImage
    {
        uint32_t             width  = 0;
        uint32_t             height = 0;
        std::vector<uint8_t> rgba; // width * height * 4
    };

    // Read `texture` back to host memory as 8-bit RGBA (one-shot readback + wait).
    // Leaves the texture in its original access/layout state.
    [[nodiscard]] Expected<HostImage> ReadbackTexture(RenderDevice& device, fg::Texture& texture);

    // Convenience: ReadbackTexture then encode an 8-bit PNG to `path`.
    [[nodiscard]] Expected<void> SaveTextureToPng(RenderDevice& device, fg::Texture& texture, const std::string& path);
} // namespace vrf
