/*
 * sampler_cache.hpp - desc-keyed sampler descriptor cache. Samplers are tiny
 * immutable objects requested repeatedly with identical state (per pass, per
 * material); the cache hands back one VriDescriptor per distinct desc and owns
 * them all.
 */
#pragma once

#include <unordered_map>

#include <vri/vri.h>

namespace vrf
{
    class RenderDevice;

    class SamplerCache
    {
    public:
        SamplerCache() = default;
        explicit SamplerCache(RenderDevice& device) : m_device {&device} {}
        ~SamplerCache();

        SamplerCache(const SamplerCache&)            = delete;
        SamplerCache& operator=(const SamplerCache&) = delete;
        SamplerCache(SamplerCache&& other) noexcept;
        SamplerCache& operator=(SamplerCache&& other) noexcept;

        // Cached create-or-return; nullptr on creation failure.
        [[nodiscard]] VriDescriptor* Get(const VriSamplerDesc&);

        void Clear() noexcept;

    private:
        RenderDevice*                                m_device = nullptr;
        std::unordered_map<size_t, VriDescriptor*>   m_samplers;
    };
} // namespace vrf
