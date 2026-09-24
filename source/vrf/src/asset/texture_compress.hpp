#pragma once

#include "vrf/asset/mesh.hpp"

namespace vrf::detail
{
    // Leaves unsupported or malformed textures unchanged. Pads partial 4x4 blocks by
    // repeating their edge texels; preserves every supplied mip and its logical size.
    bool        CompressTextureBc7(Texture& texture);
    void        CompressTexturesBc7(Mesh& mesh);
    const char* Bc7EncoderName();
} // namespace vrf::detail
