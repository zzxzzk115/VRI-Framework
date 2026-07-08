/*
 * image_loader.hpp - decode PNG / JPG / HDR into a vrf::Texture via stb_image.
 *
 * Loader contract (shared by every vrf loader): an ImportOptions struct plus a
 * Load*(path, out&) -> Expected<void>. The third-party header (stb_image) is
 * confined to the .cpp.
 */
#pragma once

#include <string_view>

#include "vrf/asset/texture.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    struct ImageImportOptions
    {
        bool flipVertically = false; // flip Y on load (GL-style origin)
        bool srgb           = false; // tag 8-bit color data as sRGB (RGBA8_SRGB)
    };

    // Decode into `out` (always expanded to 4 channels; HDR -> RGBA32_SFLOAT).
    [[nodiscard]] Expected<void> LoadImage(std::string_view path, Texture& out, const ImageImportOptions& options = {});

    [[nodiscard]] Expected<Texture> LoadImage(std::string_view path, const ImageImportOptions& options = {});
} // namespace vrf
