#include "vrf/gpu/screenshot.hpp"

#include <cstring>

#include "vrf/fg/texture.hpp"
#include "vrf/gpu/render_device.hpp"

// Single definition of the stb_image_write implementation for the whole library.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace vrf
{
    namespace
    {
        // IEEE half -> float.
        float HalfToFloat(uint16_t h)
        {
            const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
            const uint32_t exp  = (h >> 10) & 0x1Fu;
            const uint32_t mant = h & 0x3FFu;
            uint32_t       bits;
            if (exp == 0)
            {
                if (mant == 0)
                {
                    bits = sign; // +/- zero
                }
                else
                {
                    int      e = -1; // normalize a subnormal
                    uint32_t m = mant;
                    do
                    {
                        ++e;
                        m <<= 1;
                    } while ((m & 0x400u) == 0);
                    bits = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
                }
            }
            else if (exp == 0x1Fu)
            {
                bits = sign | 0x7F800000u | (mant << 13); // inf / nan
            }
            else
            {
                bits = sign | (static_cast<uint32_t>(exp - 15 + 127) << 23) | (mant << 13);
            }
            float out;
            std::memcpy(&out, &bits, sizeof(out));
            return out;
        }

        uint8_t ToU8(float v)
        {
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<uint8_t>(v * 255.0f + 0.5f);
        }

        // Bytes per texel for the formats we can read back, plus a decode tag.
        enum class Decode
        {
            Rgba16f,
            Rgba8,
            Bgra8,
            Unsupported
        };

        Decode DecodeOf(VriFormat fmt, uint32_t& texelSize)
        {
            switch (fmt)
            {
                case VriFormat_RGBA16_SFLOAT:
                    texelSize = 8;
                    return Decode::Rgba16f;
                case VriFormat_RGBA8_UNORM:
                case VriFormat_RGBA8_SRGB:
                    texelSize = 4;
                    return Decode::Rgba8;
                case VriFormat_BGRA8_UNORM:
                case VriFormat_BGRA8_SRGB:
                    texelSize = 4;
                    return Decode::Bgra8;
                default:
                    texelSize = 0;
                    return Decode::Unsupported;
            }
        }
    } // namespace

    Expected<HostImage> ReadbackTexture(RenderDevice& device, fg::Texture& texture, bool flipY)
    {
        const VriCoreInterface& core = device.Core();

        const uint32_t w = texture.GetExtent().width;
        const uint32_t h = texture.GetExtent().height;
        if (w == 0 || h == 0)
            return MakeError("ReadbackTexture: texture has zero extent");

        uint32_t     texelSize = 0;
        const Decode decode    = DecodeOf(texture.GetFormat(), texelSize);
        if (decode == Decode::Unsupported)
            return MakeError("ReadbackTexture: unsupported format (need RGBA16_SFLOAT, RGBA8, or BGRA8)");

        const uint64_t rowPitch = uint64_t {w} * texelSize; // tight packing
        const uint64_t bytes    = rowPitch * h;

        VriBuffer*          staging = nullptr;
        const VriBufferDesc bd {.size            = bytes,
                                .structureStride = 0,
                                .usage           = VriBufferUsage_TransferDst,
                                .memoryLocation  = VriMemoryLocation_HostReadback};
        if (core.CreateBuffer(device.Handle(), &bd, &staging) != VriResult_Success)
            return MakeError("ReadbackTexture: CreateBuffer (HostReadback) failed");

        // One-shot: barrier to CopySource, readback into the staging buffer, barrier back.
        VriCommandAllocator* alloc = nullptr;
        core.CreateCommandAllocator(device.Handle(), VriQueueType_Graphics, &alloc);
        VriCommandBuffer* cmd = nullptr;
        core.CreateCommandBuffer(alloc, &cmd);
        VriFence* fence = nullptr;
        core.CreateFence(device.Handle(), 0, &fence);

        core.ResetCommandAllocator(alloc);
        core.BeginCommandBuffer(cmd);

        const VriAccessLayoutStage copySrc {VriAccess_CopySourceRead, VriLayout_CopySource, VriPipelineStage_Transfer};
        VriTextureBarrierDesc      toCopy {
                 .texture = texture.Handle(), .before = texture.state, .after = copySrc, .aspect = VriImageAspect_Color};
        VriBarrierGroupDesc bg {.textures = &toCopy, .textureNum = 1};
        core.CmdBarrier(cmd, &bg);

        const VriBufferTextureCopyDesc region {
            .bufferOffset      = 0,
            .bufferRowLength   = 0, // 0 = tightly packed (w texels)
            .bufferImageHeight = 0,
            .texture           = {.baseLayer = 0, .layerNum = 1, .aspect = VriImageAspect_Color}};
        core.CmdReadbackTextureToBuffer(cmd, staging, texture.Handle(), &region);

        VriTextureBarrierDesc back {
            .texture = texture.Handle(), .before = copySrc, .after = texture.state, .aspect = VriImageAspect_Color};
        VriBarrierGroupDesc bgBack {.textures = &back, .textureNum = 1};
        core.CmdBarrier(cmd, &bgBack);

        core.EndCommandBuffer(cmd);

        VriFenceSubmitDesc signal {};
        signal.fence  = fence;
        signal.value  = 1;
        signal.stages = VriPipelineStage_AllCommands;
        VriQueueSubmitDesc submit {};
        submit.commandBuffers   = &cmd;
        submit.commandBufferNum = 1;
        submit.signalFences     = &signal;
        submit.signalFenceNum   = 1;
        core.QueueSubmit(device.GraphicsQueue(), &submit);
        core.Wait(fence, 1);

        HostImage img {.width = w, .height = h};
        img.rgba.resize(uint64_t {w} * h * 4);
        const auto* src = static_cast<const uint8_t*>(core.MapBuffer(staging, 0, bytes));
        if (src)
        {
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint8_t* srow = src + uint64_t {y} * rowPitch;
                const uint32_t dy   = flipY ? (h - 1 - y) : y;
                uint8_t*       drow = img.rgba.data() + uint64_t {dy} * w * 4;
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint8_t* s = srow + uint64_t {x} * texelSize;
                    uint8_t*       d = drow + uint64_t {x} * 4;
                    switch (decode)
                    {
                        case Decode::Rgba16f: {
                            uint16_t hp[4];
                            std::memcpy(hp, s, 8);
                            d[0] = ToU8(HalfToFloat(hp[0]));
                            d[1] = ToU8(HalfToFloat(hp[1]));
                            d[2] = ToU8(HalfToFloat(hp[2]));
                            break;
                        }
                        case Decode::Rgba8:
                            d[0] = s[0];
                            d[1] = s[1];
                            d[2] = s[2];
                            break;
                        case Decode::Bgra8:
                            d[0] = s[2];
                            d[1] = s[1];
                            d[2] = s[0];
                            break;
                        default:
                            break;
                    }
                    d[3] = 255; // opaque - the RT alpha is coverage, not PNG transparency
                }
            }
        }
        core.UnmapBuffer(staging);

        core.DestroyFence(fence);
        core.DestroyCommandAllocator(alloc);
        core.DestroyBuffer(staging);

        if (!src)
            return MakeError("ReadbackTexture: MapBuffer returned null");
        return img;
    }

    Expected<void> SaveTextureToPng(RenderDevice& device, fg::Texture& texture, const std::string& path, bool flipY)
    {
        auto img = ReadbackTexture(device, texture, flipY);
        if (!img)
            return std::unexpected(img.error());
        if (stbi_write_png(path.c_str(),
                           static_cast<int>(img->width),
                           static_cast<int>(img->height),
                           4,
                           img->rgba.data(),
                           static_cast<int>(img->width * 4)) == 0)
            return MakeError("SaveTextureToPng: stbi_write_png failed for " + path);
        return {};
    }
} // namespace vrf
