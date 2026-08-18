/*
 * transient_resources.hpp - pooled allocator for framegraph textures/buffers.
 *
 * Resources are keyed by their Desc hash and recycled across frames; entries
 * idle for kMaxIdleFrames are destroyed by update(). With N frames in flight a
 * released host-visible resource may still be read by the GPU, so entries only
 * become reusable framesInFlight-1 frames after release.
 */
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "vrf/fg/buffer.hpp"
#include "vrf/fg/texture.hpp"

namespace vrf
{
    class RenderDevice;
}

namespace vrf::fg
{
    class TransientResources
    {
    public:
        explicit TransientResources(RenderDevice&, uint32_t framesInFlight = 1);
        TransientResources(const TransientResources&)     = delete;
        TransientResources(TransientResources&&) noexcept = delete;
        ~TransientResources()                             = default;

        TransientResources& operator=(const TransientResources&)     = delete;
        TransientResources& operator=(TransientResources&&) noexcept = delete;

        struct MemoryStats
        {
            uint64_t textures {0}; // approximate, bytes
            uint64_t buffers {0};
        };
        [[nodiscard]] MemoryStats getStats() const;

        // Advance one frame: age pooled entries, evict stale ones.
        void update();

        // Destroy every pooled-idle resource NOW instead of letting update() age it out. For the
        // moment the working set changes wholesale - a render-resolution or mode switch - where
        // the stale-extent set would otherwise coexist with its replacement for kMaxIdleFrames
        // (a doubled memory peak) and then die in one heartbeat (a destruction stutter mid-play).
        // The caller owns the safety contract: only call after a device drain, when nothing
        // pooled is still referenced by an in-flight frame.
        void purge();

        [[nodiscard]] Texture* acquireTexture(const Texture::Desc&);
        void                   releaseTexture(const Texture::Desc&, Texture*);

        [[nodiscard]] Buffer* acquireBuffer(const Buffer::Desc&);
        void                  releaseBuffer(const Buffer::Desc&, Buffer*);

    private:
        RenderDevice& m_device;
        uint32_t      m_framesInFlight;
        uint64_t      m_frame {0};

        template<class T>
        struct Pool
        {
            struct Entry
            {
                T*       resource {nullptr};
                uint64_t releasedAt {0}; // frame index at release
            };
            std::vector<std::unique_ptr<T>>                     resources;
            std::unordered_map<std::size_t, std::vector<Entry>> entryGroups;
        };
        Pool<Texture> m_textures;
        Pool<Buffer>  m_buffers;
    };
} // namespace vrf::fg
