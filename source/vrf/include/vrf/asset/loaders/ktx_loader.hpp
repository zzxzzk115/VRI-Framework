/*
 * ktx_loader.hpp - load DDS / KTX(v1) via the vendored single-header dds-ktx,
 * and KTX2 via libktx (opt-in: enable the vrf_loader_ktx2 xmake option).
 *
 * These are raw loaders: file bytes -> a vrf::Texture holding the format, extent,
 * and (compressed or not) mip-0 payload. The framework does not run an asset
 * import pipeline; uploading to the GPU is the caller's / UploadTexture's concern.
 */
#pragma once

#include <string_view>

#include "vrf/asset/texture.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    // DDS or KTX(v1) container (BCn + uncompressed). Stores mip 0.
    [[nodiscard]] Expected<void> LoadKtxDds(std::string_view path, Texture& out);

    // KTX2 (incl. Basis-Universal, transcoded to RGBA8). Requires the vrf_loader_ktx2 option;
    // otherwise returns an Unsupported error.
    [[nodiscard]] Expected<void> LoadKtx2(std::string_view path, Texture& out);

    // Dispatch by extension: png/jpg/hdr -> stb, dds/ktx -> dds-ktx, ktx2 -> libktx.
    [[nodiscard]] Expected<void> LoadTexture(std::string_view path, Texture& out);
} // namespace vrf
