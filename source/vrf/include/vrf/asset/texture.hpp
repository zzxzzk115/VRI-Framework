/*
 * texture.hpp - loader-agnostic CPU-side texture.
 *
 * `format` is a VriFormat so the value doubles as the GPU upload format; `data`
 * holds the pixels tightly packed (or, for KTX/KTX2, the still-compressed blob the
 * GPU consumes directly - see `compressed`).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vri/vri.h>

namespace vrf
{
    enum class TextureFileFormat
    {
        Unknown,
        PNG,
        JPG,
        HDR,
        KTX,
        KTX2,
        DDS
    };

    // One mip level of one array layer / cube face, addressing a byte range in Texture::data.
    struct TextureSubresource
    {
        uint32_t mipLevel      = 0;
        uint32_t arrayLayer    = 0; // for cubemaps this is the face/layer index (0..arrayLayers-1)
        uint64_t offset        = 0; // byte offset into Texture::data
        uint64_t size          = 0; // byte size of this subresource
        uint32_t width         = 0; // mip width in texels
        uint32_t height        = 0; // mip height in texels
        uint32_t rowPitchBytes = 0; // bytes per (block-)row; 0 = tightly packed
    };

    struct Texture
    {
        std::string name;

        uint32_t width       = 0;
        uint32_t height      = 0;
        uint32_t depth       = 1;
        uint32_t mipLevels   = 1;
        uint32_t arrayLayers = 1; // cube = 6 * arraySize
        bool     isCubemap   = false;

        VriFormat         format     = VriFormat_Unknown;
        TextureFileFormat fileFormat = TextureFileFormat::Unknown;
        bool              compressed = false; // data is block-compressed (BCn), not raw pixels

        // All subresource bytes, concatenated. `subresources` locates each mip/layer within it.
        // When `subresources` is empty, `data` is treated as a single mip-0 / layer-0 image
        // (width x height) - the fast path for stb / glTF loads.
        std::vector<uint8_t>            data;
        std::vector<TextureSubresource> subresources;

        [[nodiscard]] bool     Empty() const noexcept { return data.empty(); }
        [[nodiscard]] uint64_t SizeBytes() const noexcept { return data.size(); }
    };
} // namespace vrf
