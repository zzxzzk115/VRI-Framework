/*
 * stereo.hpp - stereo view math shared by the XR session and the desktop
 * simulation rig. Deliberately OpenXR-free: poses and asymmetric fovs use vrf
 * types, so code written against StereoRig compiles without the OpenXR loader.
 */
#pragma once

#include "vrf/core/math.hpp"

namespace vrf::xr
{
    // Half-angles in radians, signed like XrFovf: left/down negative,
    // right/up positive for a symmetric frustum.
    struct Fov
    {
        float angleLeft {0};
        float angleRight {0};
        float angleUp {0};
        float angleDown {0};

        [[nodiscard]] static constexpr Fov Symmetric(float halfHorizontal, float halfVertical)
        {
            return {-halfHorizontal, halfHorizontal, halfVertical, -halfVertical};
        }
    };

    struct Pose
    {
        Quat orientation {1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 position {0.0f};
    };

    struct EyeView
    {
        Pose pose;         // eye pose in app space
        Fov  fov;
        Mat4 view {1.0f};  // world -> eye
        Mat4 proj {1.0f};  // eye -> clip (0..1 depth)
    };

    struct StereoViews
    {
        EyeView eye[2]; // [0] = left, [1] = right
        float   nearZ {0.05f};
        float   farZ {100.0f};
    };

    // world -> eye from an eye pose.
    [[nodiscard]] Mat4 ViewFromPose(const Pose&);

    // Asymmetric off-axis projection from tangent half-angles, 0..1 depth.
    [[nodiscard]] Mat4 ProjectionFromFov(const Fov&, float nearZ, float farZ);
} // namespace vrf::xr
