/*
 * gltf_loader.hpp - load a glTF 2.0 / GLB file into a vrf::Mesh via tinygltf.
 *
 * Extracts POSITION / NORMAL / TANGENT / TEXCOORD_0 / COLOR_0, indices, PBR
 * metallic-roughness materials, and decoded textures. Triangle primitives only;
 * node transforms are not baked (primitives are flattened in local space).
 */
#pragma once

#include <string_view>

#include "vrf/asset/mesh.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    struct GltfImportOptions
    {
        bool loadTextures = true;
    };

    [[nodiscard]] Expected<void> LoadGltf(std::string_view path, Mesh& out, const GltfImportOptions& options = {});

    [[nodiscard]] Expected<Mesh> LoadGltf(std::string_view path, const GltfImportOptions& options = {});
} // namespace vrf
