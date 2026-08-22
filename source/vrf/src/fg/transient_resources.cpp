#include "vrf/fg/transient_resources.hpp"

#include <algorithm>
#include <cassert>

#include "vrf/core/log.hpp"
#include "vrf/gpu/render_device.hpp"

namespace
{
    template<class T>
    void hashCombine(std::size_t& seed, const T& v)
    {
        seed ^= std::hash<T> {}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    [[nodiscard]] std::size_t hashDesc(const vrf::fg::Texture::Desc& desc)
    {
        std::size_t h {0};
        hashCombine(h, desc.extent.width);
        hashCombine(h, desc.extent.height);
        hashCombine(h, desc.depth);
        hashCombine(h, static_cast<uint32_t>(desc.format));
        hashCombine(h, desc.numMipLevels);
        hashCombine(h, desc.layers);
        hashCombine(h, desc.cubemap);
        hashCombine(h, static_cast<uint32_t>(desc.usage));
        return h;
    }

    [[nodiscard]] std::size_t hashDesc(const vrf::fg::Buffer::Desc& desc)
    {
        std::size_t h {0};
        hashCombine(h, static_cast<uint32_t>(desc.type));
        hashCombine(h, desc.dataSize());
        // Usage is part of identity: a concurrent-shared request must never be satisfied by a
        // pooled exclusive buffer (or vice versa).
        hashCombine(h, desc.extraUsage);
        return h;
    }

    // Bytes per texel, or per 4x4 block for the compressed formats (blockDim comes back as 4).
    // 0 for Unknown: a desc that cannot be sized should contribute nothing rather than a
    // plausible-looking lie.
    [[nodiscard]] uint32_t texelSize(const VriFormat format, uint32_t& blockDim)
    {
        blockDim = 1;
        switch (format)
        {
            case VriFormat_R8_UNORM:
            case VriFormat_R8_SNORM:
            case VriFormat_R8_UINT:
            case VriFormat_R8_SINT:
            case VriFormat_S8_UINT:
                return 1;

            case VriFormat_RG8_UNORM:
            case VriFormat_RG8_SNORM:
            case VriFormat_RG8_UINT:
            case VriFormat_RG8_SINT:
            case VriFormat_R16_UNORM:
            case VriFormat_R16_SNORM:
            case VriFormat_R16_UINT:
            case VriFormat_R16_SINT:
            case VriFormat_R16_SFLOAT:
            case VriFormat_D16_UNORM:
                return 2;

            case VriFormat_RGBA8_UNORM:
            case VriFormat_RGBA8_SRGB:
            case VriFormat_RGBA8_UINT:
            case VriFormat_RGBA8_SINT:
            case VriFormat_BGRA8_UNORM:
            case VriFormat_BGRA8_SRGB:
            case VriFormat_RG16_UNORM:
            case VriFormat_RG16_SNORM:
            case VriFormat_RG16_UINT:
            case VriFormat_RG16_SINT:
            case VriFormat_RG16_SFLOAT:
            case VriFormat_R32_UINT:
            case VriFormat_R32_SINT:
            case VriFormat_R32_SFLOAT:
            case VriFormat_RGB10A2_UNORM:
            case VriFormat_RG11B10_UFLOAT:
            case VriFormat_D32_SFLOAT:
            case VriFormat_D24_UNORM_S8_UINT:
                return 4;

            case VriFormat_RGBA16_UNORM:
            case VriFormat_RGBA16_SNORM:
            case VriFormat_RGBA16_UINT:
            case VriFormat_RGBA16_SINT:
            case VriFormat_RGBA16_SFLOAT:
            case VriFormat_RG32_UINT:
            case VriFormat_RG32_SINT:
            case VriFormat_RG32_SFLOAT:
            // Two planes plus padding wherever it is reported at all; 8 is the honest figure.
            case VriFormat_D32_SFLOAT_S8_UINT:
                return 8;

            case VriFormat_RGB32_UINT:
            case VriFormat_RGB32_SINT:
            case VriFormat_RGB32_SFLOAT:
                return 12;

            case VriFormat_RGBA32_UINT:
            case VriFormat_RGBA32_SINT:
            case VriFormat_RGBA32_SFLOAT:
                return 16;

            case VriFormat_BC1_UNORM:
            case VriFormat_BC4_UNORM:
                blockDim = 4;
                return 8;

            case VriFormat_BC2_UNORM:
            case VriFormat_BC3_UNORM:
            case VriFormat_BC5_UNORM:
            case VriFormat_BC6H_UFLOAT:
            case VriFormat_BC7_UNORM:
                blockDim = 4;
                return 16;

            // No default: a format added to VriFormat must be sized here, and the compiler is what
            // should say so - not a stat that silently keeps reporting a number.
            case VriFormat_Unknown:
            case VriFormat_Count:
            case VriFormat_MaxEnum:
                break;
        }
        return 0;
    }

    [[nodiscard]] uint64_t approximateSize(const vrf::fg::Texture::Desc& desc)
    {
        // Sized from the FORMAT. This assumed 4 bytes/texel for everything until 2026-08-21, which
        // halves every fp16 and 32-bit-pair target - and those are exactly what a deferred
        // renderer's transients are, so the figure was ~2x low precisely where it mattered and the
        // stat could not be used to reason about memory at all.
        //
        // Still approximate, and deliberately: no alignment or tiling padding, and the mip tail is
        // the 1/3 geometric estimate rather than a walk of the chain. It is a budget readout, not
        // an allocation.
        uint32_t       blockDim = 1;
        const uint64_t texel    = texelSize(desc.format, blockDim);
        const uint64_t width    = (uint64_t {desc.extent.width} + blockDim - 1) / blockDim;
        const uint64_t height   = (uint64_t {desc.extent.height} + blockDim - 1) / blockDim;

        uint64_t size = width * height * std::max(desc.depth, 1u) * texel;
        if (desc.numMipLevels > 1)
        {
            size += size / 3;
        }
        size *= std::max(desc.layers, 1u);
        if (desc.cubemap)
        {
            size *= 6;
        }
        return size;
    }
} // namespace

namespace vrf::fg
{
    namespace
    {
        // Frames an idle pooled resource survives before eviction.
        constexpr uint64_t kMaxIdleFrames = 10;

        // Evict idle entries older than maxIdle (0 = everything idle, the purge case).
        template<class Pool>
        void heartbeat(Pool& pool, const uint64_t frame, const uint64_t maxIdle)
        {
            for (auto groupsIt = pool.entryGroups.begin(); groupsIt != pool.entryGroups.end();)
            {
                auto& group = groupsIt->second;
                std::erase_if(group, [&](auto& entry) {
                    if (frame - entry.releasedAt >= maxIdle)
                    {
                        *entry.resource = {};
                        return true;
                    }
                    return false;
                });
                groupsIt = group.empty() ? pool.entryGroups.erase(groupsIt) : std::next(groupsIt);
            }

            std::erase_if(pool.resources, [](auto& r) { return !(*r); });
        }
    } // namespace

    TransientResources::TransientResources(RenderDevice& device, const uint32_t framesInFlight) :
        m_device {device}, m_framesInFlight {std::max(framesInFlight, 1u)}
    {}

    TransientResources::MemoryStats TransientResources::getStats() const
    {
        MemoryStats stats;
        for (const auto& t : m_textures.resources)
        {
            stats.textures += approximateSize(t->GetDesc());
        }
        for (const auto& b : m_buffers.resources)
        {
            stats.buffers += b->GetSize();
        }
        stats.texturesPeak = m_lastPeakTextures;
        stats.buffersPeak  = m_lastPeakBuffers;
        stats.texturesLive = m_liveTextures;
        stats.buffersLive  = m_liveBuffers;
        return stats;
    }

    void TransientResources::update()
    {
        ++m_frame;
        // Close the peak window on the frame that just ended. Seeded with what is still live
        // rather than 0, so a resource held across the boundary keeps counting.
        m_lastPeakTextures = m_peakTextures;
        m_lastPeakBuffers  = m_peakBuffers;
        m_peakTextures     = m_liveTextures;
        m_peakBuffers      = m_liveBuffers;
        heartbeat(m_textures, m_frame, kMaxIdleFrames);
        heartbeat(m_buffers, m_frame, kMaxIdleFrames);
    }

    void TransientResources::purge()
    {
        heartbeat(m_textures, m_frame, 0);
        heartbeat(m_buffers, m_frame, 0);
    }

    Texture* TransientResources::acquireTexture(const Texture::Desc& desc)
    {
        auto& group = m_textures.entryGroups[hashDesc(desc)];

        // Device-local textures are only touched by GPU commands on one queue;
        // barriers order cross-frame reuse, so pooled entries are reusable
        // immediately.
        // Live accounting brackets the framegraph's first and last use of the resource, so it
        // is charged whether the texture came from the pool or was created here.
        m_liveTextures += approximateSize(desc);
        m_peakTextures = std::max(m_peakTextures, m_liveTextures);

        if (!group.empty())
        {
            auto* texture = group.back().resource;
            group.pop_back();
            return texture;
        }

        auto created = Texture::Create(m_device, desc);
        if (!created)
        {
            LogError("fg::TransientResources: {}", created.error().message);
            return nullptr;
        }
        m_textures.resources.push_back(std::make_unique<Texture>(std::move(*created)));
        return m_textures.resources.back().get();
    }

    void TransientResources::releaseTexture(const Texture::Desc& desc, Texture* texture)
    {
        const uint64_t size = approximateSize(desc);
        m_liveTextures      = m_liveTextures > size ? m_liveTextures - size : 0;
        m_textures.entryGroups[hashDesc(desc)].push_back({texture, m_frame});
    }

    Buffer* TransientResources::acquireBuffer(const Buffer::Desc& desc)
    {
        assert(desc.dataSize() > 0);
        auto& group = m_buffers.entryGroups[hashDesc(desc)];

        // Host-visible buffers (uniforms) are map-written by the CPU, which
        // races the GPU still reading the previous frame - hold them back a
        // full frames-in-flight window. Device-local buffers are ordered by
        // barriers like textures.
        m_liveBuffers += desc.dataSize();
        m_peakBuffers = std::max(m_peakBuffers, m_liveBuffers);

        const bool     hostVisible = desc.type == BufferType::UniformBuffer;
        const uint64_t minAge      = hostVisible ? m_framesInFlight : 0;
        for (auto it = group.begin(); it != group.end(); ++it)
        {
            if (m_frame - it->releasedAt >= minAge)
            {
                auto* buffer = it->resource;
                group.erase(it);
                return buffer;
            }
        }

        auto created = Buffer::Create(m_device, desc);
        if (!created)
        {
            LogError("fg::TransientResources: {}", created.error().message);
            return nullptr;
        }
        m_buffers.resources.push_back(std::make_unique<Buffer>(std::move(*created)));
        return m_buffers.resources.back().get();
    }

    void TransientResources::releaseBuffer(const Buffer::Desc& desc, Buffer* buffer)
    {
        const uint64_t size = desc.dataSize();
        m_liveBuffers       = m_liveBuffers > size ? m_liveBuffers - size : 0;
        m_buffers.entryGroups[hashDesc(desc)].push_back({buffer, m_frame});
    }
} // namespace vrf::fg
