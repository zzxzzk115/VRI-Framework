#include "vrf/fg/transient_resources.hpp"

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
        return h;
    }

    [[nodiscard]] uint64_t approximateSize(const vrf::fg::Texture::Desc& desc)
    {
        // Rough: 4 bytes/texel + 1/3 mip overhead. Stats only, not allocation.
        uint64_t size = uint64_t {desc.extent.width} * desc.extent.height * std::max(desc.depth, 1u) * 4;
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

        template<class Pool>
        void heartbeat(Pool& pool, const uint64_t frame)
        {
            for (auto groupsIt = pool.entryGroups.begin(); groupsIt != pool.entryGroups.end();)
            {
                auto& group = groupsIt->second;
                std::erase_if(group, [&](auto& entry) {
                    if (frame - entry.releasedAt >= kMaxIdleFrames)
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
        return stats;
    }

    void TransientResources::update()
    {
        ++m_frame;
        heartbeat(m_textures, m_frame);
        heartbeat(m_buffers, m_frame);
    }

    Texture* TransientResources::acquireTexture(const Texture::Desc& desc)
    {
        auto& group = m_textures.entryGroups[hashDesc(desc)];

        // Only reuse entries the GPU is guaranteed done with.
        for (auto it = group.begin(); it != group.end(); ++it)
        {
            if (m_frame - it->releasedAt >= m_framesInFlight - 1)
            {
                auto* texture = it->resource;
                group.erase(it);
                return texture;
            }
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
        m_textures.entryGroups[hashDesc(desc)].push_back({texture, m_frame});
    }

    Buffer* TransientResources::acquireBuffer(const Buffer::Desc& desc)
    {
        assert(desc.dataSize() > 0);
        auto& group = m_buffers.entryGroups[hashDesc(desc)];

        for (auto it = group.begin(); it != group.end(); ++it)
        {
            if (m_frame - it->releasedAt >= m_framesInFlight - 1)
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
        m_buffers.entryGroups[hashDesc(desc)].push_back({buffer, m_frame});
    }
} // namespace vrf::fg
