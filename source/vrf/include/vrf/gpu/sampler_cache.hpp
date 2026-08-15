/*
 * sampler_cache.hpp - desc-keyed sampler descriptor cache. Samplers are tiny
 * immutable objects requested repeatedly with identical state (per pass, per
 * material); the cache hands back one DescriptorHandle per distinct desc and owns
 * them all. A thin wrapper over the generic ResourceCache: keyed by the full
 * desc (memcmp equality, so no hash-collision mix-ups) with RAII handles.
 */
#pragma once

#include <cstring>

#include "vrf/gpu/vri_types.hpp"
#include <vri/vri.h>

#include "vrf/gpu/resource_cache.hpp"

namespace vrf
{
    class RenderDevice;

    namespace detail
    {
        struct SamplerKey
        {
            VriSamplerDesc desc;
            bool           operator==(const SamplerKey& o) const noexcept
            {
                return std::memcmp(&desc, &o.desc, sizeof(desc)) == 0; // exact bytes -> no collision aliasing
            }
        };
        struct SamplerKeyHash
        {
            size_t operator()(const SamplerKey& k) const noexcept { return HashPodBytes(k.desc); }
        };
    } // namespace detail

    class SamplerCache
    {
    public:
        SamplerCache() = default;
        explicit SamplerCache(RenderDevice& device) : m_device {&device}, m_cache {device} {}

        SamplerCache(const SamplerCache&)                = delete;
        SamplerCache& operator=(const SamplerCache&)     = delete;
        SamplerCache(SamplerCache&&) noexcept            = default;
        SamplerCache& operator=(SamplerCache&&) noexcept = default;

        // Cached create-or-return; nullptr on creation failure.
        [[nodiscard]] DescriptorHandle* Get(const VriSamplerDesc&);

        void Clear() noexcept { m_cache.Clear(); }

    private:
        RenderDevice*                                                               m_device {nullptr};
        ResourceCache<detail::SamplerKey, DescriptorHandle, detail::SamplerKeyHash> m_cache;
    };
} // namespace vrf
