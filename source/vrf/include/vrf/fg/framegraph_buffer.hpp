/*
 * framegraph_buffer.hpp - the fg-library buffer resource type. Mirrors
 * libvultra's FrameGraphBuffer; see framegraph_texture.hpp.
 */
#pragma once

#include <string>

#include "vrf/fg/buffer.hpp"

namespace vrf::fg
{
    class FrameGraphBuffer
    {
    public:
        using Desc = Buffer::Desc;

        void create(const Desc&, void* allocator);
        void destroy(const Desc&, void* allocator);

        void preRead(const Desc&, uint32_t bits, void* ctx);
        void preWrite(const Desc&, uint32_t bits, void* ctx);

        [[nodiscard]] static std::string toString(const Desc&);

        Buffer* buffer {nullptr};
    };
} // namespace vrf::fg
