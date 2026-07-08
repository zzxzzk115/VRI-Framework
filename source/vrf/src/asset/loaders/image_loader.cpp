#include "vrf/asset/loaders/image_loader.hpp"

#include <cstring>
#include <string>

// stb_image implementation lives here (single definition for the whole library;
// gltf_loader.cpp reuses these symbols via TINYGLTF_NO_INCLUDE_STB_IMAGE).
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace vrf
{
    namespace
    {
        TextureFileFormat FileFormatFromPath(std::string_view path)
        {
            const size_t dot = path.find_last_of('.');
            if (dot == std::string_view::npos)
                return TextureFileFormat::Unknown;
            std::string ext(path.substr(dot + 1));
            for (char& ch : ext)
                ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
            if (ext == "png")
                return TextureFileFormat::PNG;
            if (ext == "jpg" || ext == "jpeg")
                return TextureFileFormat::JPG;
            if (ext == "hdr")
                return TextureFileFormat::HDR;
            return TextureFileFormat::Unknown;
        }
    } // namespace

    Expected<void> LoadImage(std::string_view path, Texture& out, const ImageImportOptions& options)
    {
        const std::string filePath(path);
        out = Texture {};

        stbi_set_flip_vertically_on_load(options.flipVertically ? 1 : 0);

        int width = 0, height = 0, channels = 0;
        if (stbi_is_hdr(filePath.c_str()))
        {
            float* pixels = stbi_loadf(filePath.c_str(), &width, &height, &channels, 4);
            if (!pixels)
                return MakeError(std::string("LoadImage(hdr) failed: ") + stbi_failure_reason());
            const size_t bytes = static_cast<size_t>(width) * height * 4 * sizeof(float);
            out.data.resize(bytes);
            std::memcpy(out.data.data(), pixels, bytes);
            stbi_image_free(pixels);
            out.format     = VriFormat_RGBA32_SFLOAT;
            out.fileFormat = TextureFileFormat::HDR;
        }
        else
        {
            stbi_uc* pixels = stbi_load(filePath.c_str(), &width, &height, &channels, 4);
            if (!pixels)
                return MakeError(std::string("LoadImage failed: ") + stbi_failure_reason());
            const size_t bytes = static_cast<size_t>(width) * height * 4;
            out.data.resize(bytes);
            std::memcpy(out.data.data(), pixels, bytes);
            stbi_image_free(pixels);
            out.format     = options.srgb ? VriFormat_RGBA8_SRGB : VriFormat_RGBA8_UNORM;
            out.fileFormat = FileFormatFromPath(path);
        }

        out.name        = filePath;
        out.width       = static_cast<uint32_t>(width);
        out.height      = static_cast<uint32_t>(height);
        out.depth       = 1;
        out.mipLevels   = 1;
        out.arrayLayers = 1;
        out.compressed  = false;
        return {};
    }

    Expected<Texture> LoadImage(std::string_view path, const ImageImportOptions& options)
    {
        Texture texture;
        auto    result = LoadImage(path, texture, options);
        if (!result)
            return std::unexpected(result.error());
        return texture;
    }
} // namespace vrf
