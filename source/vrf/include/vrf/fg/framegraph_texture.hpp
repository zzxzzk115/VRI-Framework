/*
 * framegraph_texture.hpp - the fg-library texture resource type.
 *
 * The `fg` package drives create/destroy through the transient pool and
 * preRead/preWrite at pass execution; the hooks decode the packed access bits
 * (resource_access.hpp) into VRI barriers, attachment declarations, and shader
 * bindings on the RenderContext. API mirrors libvultra's FrameGraphTexture.
 */
#pragma once

#include <string>

#include "vrf/fg/texture.hpp"

namespace vrf::fg
{
    class FrameGraphTexture
    {
    public:
        using Desc = Texture::Desc;

        void create(const Desc&, void* allocator);
        void destroy(const Desc&, void* allocator);

        void preRead(const Desc&, uint32_t bits, void* ctx) const;
        void preWrite(const Desc&, uint32_t bits, void* ctx) const;

        [[nodiscard]] static std::string toString(const Desc&);

        Texture* texture {nullptr};
    };
} // namespace vrf::fg
