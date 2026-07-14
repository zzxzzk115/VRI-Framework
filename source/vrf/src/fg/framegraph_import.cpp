#include "vrf/fg/framegraph_import.hpp"

#include <cassert>

#include <fg/FrameGraph.hpp>

#include "vrf/fg/framegraph_texture.hpp"

namespace vrf::fg
{
    FrameGraphResource importTexture(FrameGraph& fg, const std::string_view name, Texture* texture)
    {
        assert(texture && *texture);
        return fg.import <FrameGraphTexture>(name, texture->GetDesc(), {texture});
    }
} // namespace vrf::fg
