/*
 * framegraph_import.hpp - import an externally-owned fg::Texture (swapchain
 * image, XR eye target, persistent history buffer) into a frame's graph.
 */
#pragma once

#include <string_view>

#include <fg/Fwd.hpp>

#include "vrf/fg/texture.hpp"

namespace vrf::fg
{
    [[nodiscard]] FrameGraphResource importTexture(FrameGraph&, std::string_view name, Texture*);
} // namespace vrf::fg
