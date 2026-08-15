/*
 * resource_cache.hpp - a generic desc-keyed cache of RAII GPU resources.
 *
 * The reusable caching form behind SamplerCache (and, later, pipeline / pipeline-layout caches):
 * GPU objects that are immutable and requested repeatedly with identical state get created once
 * and shared. Keyed by the KEY ITSELF (with real hash + equality) - not by a precomputed hash
 * value, which would let a hash collision silently return the wrong resource - and it owns the
 * handles as vrf::Unique<T>, so Clear()/destruction free them with no hand-written loop.
 *
 *   ResourceCache<SamplerKey, DescriptorHandle> cache {device};
 *   DescriptorHandle* s = cache.GetOrCreate(key, [&]() -> Expected<DescriptorHandle*> { ...create... });
 */
#pragma once

#include "vrf/gpu/vri_types.hpp"

#include <cstddef>
#include <unordered_map>

#include "vrf/core/log.hpp"
#include "vrf/core/result.hpp"
#include "vrf/gpu/handle.hpp"

namespace vrf
{
    class RenderDevice;

    // FNV-1a over a trivially-copyable POD's bytes - a convenience for building key hashers.
    // (Equality must still compare the bytes; the hash only buckets.)
    template<class T>
    [[nodiscard]] inline size_t HashPodBytes(const T& value) noexcept
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        size_t      hash  = 14695981039346656037ull;
        for (size_t i = 0; i < sizeof(T); ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    template<class Key, class Handle, class Hash = std::hash<Key>, class Eq = std::equal_to<Key>>
    class ResourceCache
    {
    public:
        ResourceCache() = default;
        explicit ResourceCache(RenderDevice& device) : m_device {&device} {}

        ResourceCache(const ResourceCache&)                = delete;
        ResourceCache& operator=(const ResourceCache&)     = delete;
        ResourceCache(ResourceCache&&) noexcept            = default;
        ResourceCache& operator=(ResourceCache&&) noexcept = default;

        // Return the cached handle for `key`, or create it via `factory` (which returns
        // Expected<Handle*>) and cache it as a Unique<Handle>. Null on creation failure.
        template<class Factory>
        [[nodiscard]] Handle* GetOrCreate(const Key& key, Factory&& factory)
        {
            if (const auto it = m_map.find(key); it != m_map.end())
            {
                return it->second.get();
            }
            auto created = factory();
            if (!created)
            {
                LogError("ResourceCache: create failed ({})", created.error().message);
                return nullptr;
            }
            auto [it, _] = m_map.emplace(key, Unique<Handle> {*m_device, *created});
            return it->second.get();
        }

        void                      Clear() noexcept { m_map.clear(); } // Unique frees each entry
        [[nodiscard]] std::size_t Size() const noexcept { return m_map.size(); }

    private:
        RenderDevice*                                     m_device {nullptr};
        std::unordered_map<Key, Unique<Handle>, Hash, Eq> m_map;
    };
} // namespace vrf
