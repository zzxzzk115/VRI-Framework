#include "vrf/asset/mesh.hpp"

#include <algorithm>
#include <limits>

namespace vrf
{
    void Mesh::ComputeBounds()
    {
        if (positions.empty())
        {
            boundsMin = {0.0f, 0.0f, 0.0f};
            boundsMax = {0.0f, 0.0f, 0.0f};
            return;
        }

        constexpr float kMax = std::numeric_limits<float>::max();
        boundsMin            = {kMax, kMax, kMax};
        boundsMax            = {-kMax, -kMax, -kMax};
        for (const Position& p : positions)
        {
            boundsMin = glm::min(boundsMin, p);
            boundsMax = glm::max(boundsMax, p);
        }
    }

    std::vector<Tangent> GenerateTangents(const Mesh& mesh)
    {
        const size_t           vcount  = mesh.positions.size();
        const bool             hasUV   = mesh.texCoords0.size() == vcount;
        const bool             hasNorm = mesh.normals.size() == vcount;
        std::vector<glm::vec3> tan1(vcount, glm::vec3(0.0f));
        std::vector<glm::vec3> tan2(vcount, glm::vec3(0.0f));

        const auto processTri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
            if (i0 >= vcount || i1 >= vcount || i2 >= vcount)
                return;
            const glm::vec3 e1  = mesh.positions[i1] - mesh.positions[i0];
            const glm::vec3 e2  = mesh.positions[i2] - mesh.positions[i0];
            const glm::vec2 uv0 = hasUV ? mesh.texCoords0[i0] : glm::vec2(0.0f);
            const glm::vec2 uv1 = hasUV ? mesh.texCoords0[i1] : glm::vec2(0.0f);
            const glm::vec2 uv2 = hasUV ? mesh.texCoords0[i2] : glm::vec2(0.0f);
            const glm::vec2 d1  = uv1 - uv0;
            const glm::vec2 d2  = uv2 - uv0;
            const float     det = d1.x * d2.y - d2.x * d1.y;
            const float     r   = glm::abs(det) > 1e-12f ? 1.0f / det : 0.0f;
            const glm::vec3 s   = (e1 * d2.y - e2 * d1.y) * r;
            const glm::vec3 t   = (e2 * d1.x - e1 * d2.x) * r;
            tan1[i0] += s;
            tan1[i1] += s;
            tan1[i2] += s;
            tan2[i0] += t;
            tan2[i1] += t;
            tan2[i2] += t;
        };

        if (!mesh.indices.empty())
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                processTri(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]);
        else
            for (size_t i = 0; i + 2 < vcount; i += 3)
                processTri(static_cast<uint32_t>(i), static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i + 2));

        std::vector<Tangent> out(vcount);
        for (size_t i = 0; i < vcount; ++i)
        {
            const glm::vec3 n       = hasNorm ? mesh.normals[i] : glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 t       = tan1[i];
            glm::vec3       tangent = t - n * glm::dot(n, t); // Gram-Schmidt orthogonalize
            const float     len     = glm::length(tangent);
            tangent                 = len > 1e-8f ? tangent / len : glm::vec3(1.0f, 0.0f, 0.0f);
            const float w           = glm::dot(glm::cross(n, t), tan2[i]) < 0.0f ? -1.0f : 1.0f; // handedness
            out[i]                  = glm::vec4(tangent, w);
        }
        return out;
    }
} // namespace vrf
