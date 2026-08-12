/*
 * mesh.hpp - loader-agnostic mesh in de-interleaved (SoA) form.
 *
 * Attribute arrays are parallel and index-aligned; `attributes` records which are
 * populated. Submeshes carve the shared index buffer into material-tagged ranges.
 * Referenced textures are owned here and addressed by index from Material.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vrf/asset/material.hpp"
#include "vrf/asset/texture.hpp"
#include "vrf/asset/vertex.hpp"
#include "vrf/core/math.hpp"

namespace vrf
{
    struct SubMesh
    {
        std::string name;
        uint32_t    indexOffset   = 0;
        uint32_t    indexCount    = 0;
        uint32_t    vertexOffset  = 0;
        uint32_t    vertexCount   = 0;
        int         materialIndex = -1;
    };

    struct Mesh
    {
        std::string name;

        VertexAttribute attributes = VertexAttribute::None;

        std::vector<Position> positions;
        std::vector<Normal>   normals;
        std::vector<Tangent>  tangents;
        std::vector<Color>    colors;
        std::vector<TexCoord> texCoords0;
        std::vector<TexCoord> texCoords1;
        std::vector<uint32_t> indices;

        std::vector<SubMesh>  subMeshes;
        std::vector<Material> materials;
        std::vector<Texture>  textures;

        Vec3 boundsMin = {0.0f, 0.0f, 0.0f};
        Vec3 boundsMax = {0.0f, 0.0f, 0.0f};

        [[nodiscard]] uint32_t VertexCount() const noexcept { return static_cast<uint32_t>(positions.size()); }
        [[nodiscard]] uint32_t IndexCount() const noexcept { return static_cast<uint32_t>(indices.size()); }

        // Recompute boundsMin/Max from positions (no-op if there are none).
        void ComputeBounds();
    };

    // Per-vertex tangents (xyz + handedness in w) from positions/uvs/normals, Lengyel's method.
    // UploadMesh calls this when a mesh has normals + uvs but no TANGENT; the bake cache calls it
    // once offline so the cached mesh arrives with tangents already populated.
    [[nodiscard]] std::vector<Tangent> GenerateTangents(const Mesh& mesh);
} // namespace vrf
