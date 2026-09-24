#include "asset/texture_compress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#if defined(VRF_ENABLE_BAKE_BC7)
#if defined(VRF_BC7_ISPC)
#include "bc7e_ispc.h"
#else
#include "bc7enc.h"
#endif
#endif

namespace vrf::detail
{
    const char* Bc7EncoderName()
    {
#if defined(VRF_BC7_ISPC)
        return "bc7e-ispc-fast-v1";
#elif defined(VRF_ENABLE_BAKE_BC7)
        return "bc7enc-direct-v1";
#else
        return "rgba8-v1";
#endif
    }

    bool CompressTextureBc7(Texture& texture)
    {
#if defined(VRF_ENABLE_BAKE_BC7)
        if (texture.compressed || texture.data.empty() || texture.depth != 1 || texture.isCubemap ||
            texture.arrayLayers != 1 || texture.width == 0 || texture.height == 0 ||
            (texture.format != VriFormat_RGBA8_UNORM && texture.format != VriFormat_RGBA8_SRGB))
            return false;

        const uint32_t levels = std::max(texture.mipLevels, 1u);
        if (levels > 32 || (texture.subresources.empty() && levels != 1))
            return false;
        struct Level
        {
            uint64_t offset, pitch, blocksX;
        };
        std::vector<Level>              inputs;
        std::vector<TextureSubresource> subs;
        uint64_t                        size = 0;
        for (uint32_t mip = 0; mip < levels; ++mip)
        {
            const uint32_t width  = std::max(texture.width >> mip, 1u);
            const uint32_t height = std::max(texture.height >> mip, 1u);
            uint64_t       offset = 0, pitch = uint64_t(width) * 4, bytes = texture.data.size();
            if (!texture.subresources.empty())
            {
                const auto it = std::find_if(texture.subresources.begin(),
                                             texture.subresources.end(),
                                             [mip](const auto& s) { return s.mipLevel == mip && s.arrayLayer == 0; });
                if (it == texture.subresources.end() || it->width != width || it->height != height)
                    return false;
                offset = it->offset;
                pitch  = it->rowPitchBytes ? it->rowPitchBytes : pitch;
                bytes  = it->size;
            }
            if (pitch < uint64_t(width) * 4 || offset > texture.data.size() || bytes > texture.data.size() - offset ||
                (uint64_t(height) - 1) * pitch + uint64_t(width) * 4 > bytes)
                return false;
            const uint64_t blocksX = (uint64_t(width) + 3) / 4;
            const uint64_t blocks  = blocksX * ((uint64_t(height) + 3) / 4);
            if (blocks > (std::numeric_limits<size_t>::max() - size) / 16)
                return false;
            inputs.push_back({offset, pitch, blocksX});
            TextureSubresource sub;
            sub.mipLevel = mip;
            sub.offset   = size;
            sub.size     = blocks * 16;
            sub.width    = width;
            sub.height   = height;
            subs.push_back(sub);
            size += sub.size;
        }

        static std::once_flag init;
#if defined(VRF_BC7_ISPC)
        std::call_once(init, [] { ispc::bc7e_compress_block_init(); });
        ispc::bc7e_compress_block_params params;
        ispc::bc7e_compress_block_params_init_fast(&params, texture.format == VriFormat_RGBA8_SRGB);
#else
        std::call_once(init, [] { bc7enc_compress_block_init(); });
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        if (texture.format != VriFormat_RGBA8_SRGB)
            bc7enc_compress_block_params_init_linear_weights(&params);
#endif
        std::vector<uint8_t> data(static_cast<size_t>(size));
        // Block batches keep scratch bounded and parallelize even a single large image.
        // The encoder never needs a second full RGBA image or an intermediate UASTC blob.
        for (size_t mip = 0; mip < subs.size(); ++mip)
        {
            const auto&           sub    = subs[mip];
            const auto&           in     = inputs[mip];
            const uint64_t        blocks = sub.size / 16;
            std::atomic<uint64_t> next {0};
            const auto            encode = [&] {
                std::array<uint32_t, 64 * 16> pixels;
                std::array<uint64_t, 64 * 2>  encoded;
                for (uint64_t first = next.fetch_add(64); first < blocks; first = next.fetch_add(64))
                {
                    const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(64, blocks - first));
                    for (uint32_t b = 0; b < count; ++b)
                    {
                        const uint64_t bx = (first + b) % in.blocksX;
                        const uint64_t by = (first + b) / in.blocksX;
                        for (uint32_t y = 0; y < 4; ++y)
                            for (uint32_t x = 0; x < 4; ++x)
                            {
                                const auto sx = std::min<uint64_t>(bx * 4 + x, sub.width - 1);
                                const auto sy = std::min<uint64_t>(by * 4 + y, sub.height - 1);
                                std::memcpy(&pixels[b * 16 + y * 4 + x],
                                            texture.data.data() + in.offset + sy * in.pitch + sx * 4,
                                            4);
                            }
                    }
#if defined(VRF_BC7_ISPC)
                    ispc::bc7e_compress_blocks(count, encoded.data(), pixels.data(), &params);
#else
                    for (uint32_t b = 0; b < count; ++b)
                        bc7enc_compress_block(encoded.data() + b * 2, pixels.data() + b * 16, &params);
#endif
                    std::memcpy(data.data() + sub.offset + first * 16, encoded.data(), count * 16);
                }
            };
            const unsigned            workers = static_cast<unsigned>(std::min<uint64_t>(
                std::max(1u, std::thread::hardware_concurrency()), std::max<uint64_t>(1, blocks / 1024)));
            std::vector<std::jthread> pool;
            for (unsigned i = 1; i < workers; ++i)
                pool.emplace_back(encode);
            encode();
        }
        texture.data         = std::move(data);
        texture.subresources = std::move(subs);
        texture.mipLevels    = levels;
        texture.format       = VriFormat_BC7_UNORM;
        texture.compressed   = true;
        return true;
#else
        (void)texture;
        return false;
#endif
    }

    void CompressTexturesBc7(Mesh& mesh)
    {
        for (auto& texture : mesh.textures)
            CompressTextureBc7(texture);
    }
} // namespace vrf::detail
