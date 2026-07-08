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
} // namespace vrf
