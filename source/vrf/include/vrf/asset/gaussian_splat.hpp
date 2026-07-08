/*
 * gaussian_splat.hpp - 3D Gaussian Splatting data (loader-agnostic).
 *
 * GaussianSplatPoint is a 64-byte, 16-byte-aligned record uploadable to the GPU
 * verbatim (raw logit opacity + log-space scale + quaternion rotation + SH DC
 * term); higher-order SH coefficients live in the separate `sh` array. Layout
 * mirrors libvultra/vasset so a splat renderer can consume it directly.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vrf/core/math.hpp"

namespace vrf
{
    struct alignas(16) GaussianSplatPoint
    {
        Vec3  position; // world-space center
        float opacity;  // raw logit (renderer applies sigmoid)
        Vec3  scale;    // log-space (renderer applies exp)
        float pad0;
        Vec4  rotation; // quaternion xyzw
        Vec3  shDC;     // SH DC term (band 0)
        float pad1;
    };
    static_assert(sizeof(GaussianSplatPoint) == 64, "GaussianSplatPoint must stay 64 bytes for verbatim GPU upload");

    struct GaussianSplat
    {
        std::string name;

        int32_t numPoints   = 0;
        int32_t shDegree    = 0; // 0..3
        bool    antialiased = false;

        std::vector<GaussianSplatPoint> splats;
        std::vector<float>              sh; // higher-order SH coefficients (band > 0), flattened

        [[nodiscard]] bool Empty() const noexcept { return splats.empty(); }
    };
} // namespace vrf
