#include "asset/texture_compress.hpp"
#include "bc7decomp.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ktx.h>
#include <vector>
#include <vrf/asset/loaders/image_loader.hpp>
std::vector<uint8_t> old_encode(const vrf::Texture& source)
{
    ktxTextureCreateInfo info {};
    info.vkFormat      = 37;
    info.baseWidth     = source.width;
    info.baseHeight    = source.height;
    info.baseDepth     = 1;
    info.numDimensions = 2;
    info.numLevels     = 1;
    info.numLayers     = 1;
    info.numFaces      = 1;
    ktxTexture2* ktx   = nullptr;
    if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktx) != KTX_SUCCESS)
        std::abort();
    ktxTexture_SetImageFromMemory(ktxTexture(ktx), 0, 0, 0, source.data.data(), source.data.size());
    ktxBasisParams params {};
    params.structSize  = sizeof(params);
    params.uastc       = KTX_TRUE;
    params.threadCount = 1;
    if (ktxTexture2_CompressBasisEx(ktx, &params) != KTX_SUCCESS ||
        ktxTexture2_TranscodeBasis(ktx, KTX_TTF_BC7_RGBA, 0) != KTX_SUCCESS)
        std::abort();
    const auto*          data = ktxTexture_GetData(ktxTexture(ktx));
    std::vector<uint8_t> result(data, data + ktxTexture_GetDataSize(ktxTexture(ktx)));
    ktxTexture_Destroy(ktxTexture(ktx));
    return result;
}
double psnr(const vrf::Texture& source, const std::vector<uint8_t>& encoded)
{
    double   error = 0;
    size_t   count = 0;
    unsigned bx    = (source.width + 3) / 4;
    for (size_t i = 0; i < encoded.size() / 16; ++i)
    {
        bc7decomp::color_rgba pixels[16];
        if (!bc7decomp::unpack_bc7(encoded.data() + i * 16, pixels))
            std::abort();
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x)
            {
                unsigned sx = (i % bx) * 4 + x, sy = (i / bx) * 4 + y;
                if (sx >= source.width || sy >= source.height)
                    continue;
                for (unsigned c = 0; c < 4; ++c)
                {
                    double d =
                        int(pixels[y * 4 + x].m_comps[c]) - int(source.data[(size_t(sy) * source.width + sx) * 4 + c]);
                    error += d * d;
                    ++count;
                }
            }
    }
    return 10 * std::log10(255. * 255. / (error / count));
}
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        vrf::Texture source;
        if (!vrf::LoadImage(argv[i], source))
            return 1;
        const auto start  = std::chrono::steady_clock::now();
        auto       before = old_encode(source);
        const auto split  = std::chrono::steady_clock::now();
        auto       after  = source;
        if (!vrf::detail::CompressTextureBc7(after))
            return 2;
        const auto finish = std::chrono::steady_clock::now();
        std::printf("%s %ux%u old_seconds=%.4f direct_seconds=%.4f old_psnr=%.3f direct_psnr=%.3f\n",
                    argv[i],
                    source.width,
                    source.height,
                    std::chrono::duration<double>(split - start).count(),
                    std::chrono::duration<double>(finish - split).count(),
                    psnr(source, before),
                    psnr(source, after.data));
        std::fflush(stdout);
    }
}
