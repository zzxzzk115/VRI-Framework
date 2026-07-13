#include "vrf/gpu/sampler_cache.hpp"

#include <bit>
#include <cstring>
#include <utility>

#include "vrf/core/log.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    namespace
    {
        [[nodiscard]] size_t hashDesc(const VriSamplerDesc& desc)
        {
            // VriSamplerDesc is a flat POD; FNV-1a over its bytes.
            const auto* bytes = reinterpret_cast<const unsigned char*>(&desc);
            size_t      hash  = 14695981039346656037ull;
            for (size_t i = 0; i < sizeof(desc); ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
            return hash;
        }
    } // namespace

    SamplerCache::~SamplerCache() { Clear(); }

    SamplerCache::SamplerCache(SamplerCache&& other) noexcept :
        m_device {std::exchange(other.m_device, nullptr)}, m_samplers {std::move(other.m_samplers)}
    {
        other.m_samplers.clear();
    }

    SamplerCache& SamplerCache::operator=(SamplerCache&& other) noexcept
    {
        if (this != &other)
        {
            Clear();
            m_device    = std::exchange(other.m_device, nullptr);
            m_samplers  = std::move(other.m_samplers);
            other.m_samplers.clear();
        }
        return *this;
    }

    VriDescriptor* SamplerCache::Get(const VriSamplerDesc& desc)
    {
        const size_t key = hashDesc(desc);
        if (const auto it = m_samplers.find(key); it != m_samplers.end())
        {
            return it->second;
        }

        VriDescriptor* sampler = nullptr;
        if (const auto r = m_device->Core().CreateSampler(m_device->Handle(), &desc, &sampler); !Succeeded(r))
        {
            LogError("SamplerCache: CreateSampler failed ({})", ToString(r));
            return nullptr;
        }
        m_samplers.emplace(key, sampler);
        return sampler;
    }

    void SamplerCache::Clear() noexcept
    {
        if (m_device)
        {
            for (auto& [_, sampler] : m_samplers)
            {
                m_device->Core().DestroyDescriptor(sampler);
            }
        }
        m_samplers.clear();
    }
} // namespace vrf
