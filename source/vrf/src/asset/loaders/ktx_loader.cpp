#include "vrf/asset/loaders/ktx_loader.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "vrf/asset/loaders/image_loader.hpp"

#define DDSKTX_IMPLEMENT
#include "thirdparty/dds-ktx.h"

#if defined(VRF_ENABLE_KTX2)
#include <ktx.h>
#endif

namespace vrf
{
    namespace
    {
        std::vector<uint8_t> ReadBinaryFile(const std::string& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            if (size <= 0)
                return {};
            file.seekg(0);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            file.read(reinterpret_cast<char*>(bytes.data()), size);
            return bytes;
        }

        std::string LowerExtension(std::string_view path)
        {
            const size_t dot = path.find_last_of('.');
            if (dot == std::string_view::npos)
                return {};
            std::string ext(path.substr(dot + 1));
            for (char& ch : ext)
                ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
            return ext;
        }

        VriFormat DdsKtxToVriFormat(ddsktx_format format)
        {
            switch (format)
            {
                case DDSKTX_FORMAT_BC1:
                    return VriFormat_BC1_UNORM;
                case DDSKTX_FORMAT_BC2:
                    return VriFormat_BC2_UNORM;
                case DDSKTX_FORMAT_BC3:
                    return VriFormat_BC3_UNORM;
                case DDSKTX_FORMAT_BC4:
                    return VriFormat_BC4_UNORM;
                case DDSKTX_FORMAT_BC5:
                    return VriFormat_BC5_UNORM;
                case DDSKTX_FORMAT_BC6H:
                    return VriFormat_BC6H_UFLOAT;
                case DDSKTX_FORMAT_BC7:
                    return VriFormat_BC7_UNORM;
                case DDSKTX_FORMAT_R8:
                    return VriFormat_R8_UNORM;
                case DDSKTX_FORMAT_RG8:
                    return VriFormat_RG8_UNORM;
                case DDSKTX_FORMAT_RGBA8:
                    return VriFormat_RGBA8_UNORM;
                case DDSKTX_FORMAT_BGRA8:
                    return VriFormat_BGRA8_UNORM;
                case DDSKTX_FORMAT_RG16:
                    return VriFormat_RG16_UNORM;
                case DDSKTX_FORMAT_RGBA16:
                    return VriFormat_RGBA16_UNORM;
                case DDSKTX_FORMAT_R16F:
                    return VriFormat_R16_SFLOAT;
                case DDSKTX_FORMAT_RG16F:
                    return VriFormat_RG16_SFLOAT;
                case DDSKTX_FORMAT_RGBA16F:
                    return VriFormat_RGBA16_SFLOAT;
                case DDSKTX_FORMAT_R32F:
                    return VriFormat_R32_SFLOAT;
                case DDSKTX_FORMAT_RGB10A2:
                    return VriFormat_RGB10A2_UNORM;
                case DDSKTX_FORMAT_RG11B10F:
                    return VriFormat_RG11B10_UFLOAT;
                default:
                    return VriFormat_Unknown; // RGB8 / ETC / PVRTC / ASTC / ATC: unsupported by VRI
            }
        }
    } // namespace

    Expected<void> LoadKtxDds(std::string_view path, Texture& out)
    {
        const std::vector<uint8_t> bytes = ReadBinaryFile(std::string(path));
        if (bytes.empty())
            return MakeError(std::string("LoadKtxDds: cannot read file: ") + std::string(path));

        ddsktx_texture_info info {};
        if (!ddsktx_parse(&info, bytes.data(), static_cast<int>(bytes.size()), nullptr))
            return MakeError("LoadKtxDds: not a valid DDS/KTX container");

        const VriFormat format = DdsKtxToVriFormat(info.format);
        if (format == VriFormat_Unknown)
            return MakeError(std::string("LoadKtxDds: unsupported format ") + ddsktx_format_str(info.format));
        if (info.depth > 1)
            return MakeError("LoadKtxDds: 3D/volume textures are not yet supported");

        const bool cubemap   = (info.flags & DDSKTX_TEXTURE_FLAG_CUBEMAP) != 0;
        const int  numLayers = info.num_layers > 0 ? info.num_layers : 1;
        const int  numFaces  = cubemap ? 6 : 1;
        const int  numMips   = info.num_mips > 0 ? info.num_mips : 1;

        out             = Texture {};
        out.name        = std::string(path);
        out.width       = static_cast<uint32_t>(info.width);
        out.height      = static_cast<uint32_t>(info.height);
        out.depth       = 1;
        out.mipLevels   = static_cast<uint32_t>(numMips);
        out.arrayLayers = static_cast<uint32_t>(numLayers * numFaces);
        out.isCubemap   = cubemap;
        out.format      = format;
        out.compressed  = ddsktx_format_compressed(info.format);
        out.fileFormat  = LowerExtension(path) == "dds" ? TextureFileFormat::DDS : TextureFileFormat::KTX;

        // Gather every subresource (layer x face x mip) into one blob + a locator table.
        for (int layer = 0; layer < numLayers; ++layer)
        {
            for (int face = 0; face < numFaces; ++face)
            {
                for (int mip = 0; mip < numMips; ++mip)
                {
                    ddsktx_sub_data sub {};
                    ddsktx_get_sub(&info, &sub, bytes.data(), static_cast<int>(bytes.size()), layer, face, mip);
                    if (!sub.buff || sub.size_bytes <= 0)
                        continue;

                    TextureSubresource entry;
                    entry.mipLevel      = static_cast<uint32_t>(mip);
                    entry.arrayLayer    = static_cast<uint32_t>(layer * numFaces + face);
                    entry.offset        = out.data.size();
                    entry.size          = static_cast<uint64_t>(sub.size_bytes);
                    entry.width         = static_cast<uint32_t>(sub.width);
                    entry.height        = static_cast<uint32_t>(sub.height);
                    entry.rowPitchBytes = static_cast<uint32_t>(sub.row_pitch_bytes);

                    const auto* first = static_cast<const uint8_t*>(sub.buff);
                    out.data.insert(out.data.end(), first, first + sub.size_bytes);
                    out.subresources.push_back(entry);
                }
            }
        }
        if (out.data.empty())
            return MakeError("LoadKtxDds: no subresource data");
        return {};
    }

#if defined(VRF_ENABLE_KTX2)
    Expected<void> LoadKtx2(std::string_view path, Texture& out)
    {
        ktxTexture2*           ktx = nullptr;
        const ktx_error_code_e r =
            ktxTexture2_CreateFromNamedFile(std::string(path).c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);
        if (r != KTX_SUCCESS)
            return MakeError(std::string("LoadKtx2: ") + ktxErrorString(r));

        // Transcode Basis-Universal payloads to plain RGBA8 so the result is directly uploadable.
        if (ktxTexture2_NeedsTranscoding(ktx))
        {
            const ktx_error_code_e t = ktxTexture2_TranscodeBasis(ktx, KTX_TTF_RGBA32, 0);
            if (t != KTX_SUCCESS)
            {
                ktxTexture_Destroy(ktxTexture(ktx));
                return MakeError(std::string("LoadKtx2: transcode failed: ") + ktxErrorString(t));
            }
        }

        const ktx_uint8_t* data   = ktxTexture_GetData(ktxTexture(ktx));
        const uint32_t     mips   = ktx->numLevels > 0 ? ktx->numLevels : 1;
        const uint32_t     faces  = ktx->numFaces > 0 ? ktx->numFaces : 1;
        const uint32_t     layers = ktx->numLayers > 0 ? ktx->numLayers : 1;

        out             = Texture {};
        out.name        = std::string(path);
        out.width       = ktx->baseWidth;
        out.height      = ktx->baseHeight;
        out.depth       = ktx->baseDepth > 0 ? ktx->baseDepth : 1;
        out.mipLevels   = mips;
        out.arrayLayers = layers * faces;
        out.isCubemap   = ktx->isCubemap != 0;
        out.format      = VriFormat_RGBA8_UNORM; // after RGBA32 transcode (or already-RGBA8 KTX2)
        out.compressed  = false;
        out.fileFormat  = TextureFileFormat::KTX2;

        for (uint32_t layer = 0; layer < layers; ++layer)
        {
            for (uint32_t face = 0; face < faces; ++face)
            {
                for (uint32_t mip = 0; mip < mips; ++mip)
                {
                    ktx_size_t imageOffset = 0;
                    ktxTexture_GetImageOffset(ktxTexture(ktx), mip, layer, face, &imageOffset);
                    const ktx_size_t imageSize = ktxTexture_GetImageSize(ktxTexture(ktx), mip);

                    TextureSubresource entry;
                    entry.mipLevel   = mip;
                    entry.arrayLayer = layer * faces + face;
                    entry.offset     = out.data.size();
                    entry.size       = imageSize;
                    entry.width      = out.width >> mip ? out.width >> mip : 1;
                    entry.height     = out.height >> mip ? out.height >> mip : 1;
                    out.data.insert(out.data.end(), data + imageOffset, data + imageOffset + imageSize);
                    out.subresources.push_back(entry);
                }
            }
        }

        ktxTexture_Destroy(ktxTexture(ktx));
        return {};
    }
#else
    Expected<void> LoadKtx2(std::string_view /*path*/, Texture& /*out*/)
    {
        return MakeError(
            VriResult_Unsupported, "LoadKtx2", "KTX2 support is not compiled in (enable the vrf_loader_ktx2 option)");
    }
#endif

    Expected<void> LoadTexture(std::string_view path, Texture& out)
    {
        const std::string ext = LowerExtension(path);
        if (ext == "dds" || ext == "ktx")
            return LoadKtxDds(path, out);
        if (ext == "ktx2")
            return LoadKtx2(path, out);
        // png / jpg / jpeg / hdr and friends
        return LoadImage(path, out);
    }
} // namespace vrf
