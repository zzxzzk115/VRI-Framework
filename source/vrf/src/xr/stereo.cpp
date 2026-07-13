#include "vrf/xr/stereo.hpp"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace vrf::xr
{
    Mat4 ViewFromPose(const Pose& pose)
    {
        const Mat4 world = glm::translate(Mat4 {1.0f}, pose.position) * glm::mat4_cast(pose.orientation);
        return glm::inverse(world);
    }

    Mat4 ProjectionFromFov(const Fov& fov, const float nearZ, const float farZ)
    {
        const float tanLeft  = std::tan(fov.angleLeft);
        const float tanRight = std::tan(fov.angleRight);
        const float tanUp    = std::tan(fov.angleUp);
        const float tanDown  = std::tan(fov.angleDown);

        const float width  = tanRight - tanLeft;
        const float height = tanUp - tanDown;

        // Right-handed, looking down -Z, depth 0..1 (Vulkan-style; VRI's other
        // backends convert NDC internally).
        Mat4 proj {0.0f};
        proj[0][0] = 2.0f / width;
        proj[1][1] = 2.0f / height;
        proj[2][0] = (tanRight + tanLeft) / width;
        proj[2][1] = (tanUp + tanDown) / height;
        proj[2][2] = farZ / (nearZ - farZ);
        proj[2][3] = -1.0f;
        proj[3][2] = (farZ * nearZ) / (nearZ - farZ);
        return proj;
    }
} // namespace vrf::xr
