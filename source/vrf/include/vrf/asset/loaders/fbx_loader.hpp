#pragma once

#include <string_view>

#include "vrf/asset/mesh.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    enum class FbxMaterialConvention
    {
        Phong,
        // NVIDIA ORCA: diffuse=base color, specular RGB=occlusion/roughness/metalness.
        // These assets use placeholder Phong factors; they must not modulate the maps.
        OrcaMetallicRoughness
    };

    struct FbxImportOptions
    {
        bool loadTextures    = true;
        bool flipTexCoordY   = true;
        bool convertToMeters = true;
        // Source normal-map convention before any UV flip. Tangent handedness
        // accounts for both choices, avoiding a double inversion of green.
        bool                  directXNormalMaps  = false;
        FbxMaterialConvention materialConvention = FbxMaterialConvention::Phong;
    };

    // Static triangle/quadrilateral meshes. Bakes node/geometric transforms; keeps
    // the source axis convention (callers may apply their scene transform).
    // Missing referenced textures and unsupported polygons are errors, never white fallbacks.
    // Available on every build; returns an error when vrf_loader_fbx is disabled.
    [[nodiscard]] Expected<void> LoadFbx(std::string_view path, Mesh& out, const FbxImportOptions& options = {});
} // namespace vrf
