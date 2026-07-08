/*
 * gaussian_splat_loader.hpp - 3D Gaussian Splatting loaders.
 *
 * All formats are decoded by the vendored GaussForge (+ Niantic spz) stack under external/:
 *   .ply / .compressed.ply  3DGS point clouds (the canonical training output + compressed).
 *   .spz                     Niantic SPZ.
 *   .splat                   antimatter15 format.
 *   .ksplat                  mkkellogg ksplat.
 * Each maps onto GaussianSplatPoint (raw logit opacity, log-space scale, quaternion, SH DC)
 * with higher-order SH in `sh`. No coordinate-convention change is applied - values are raw.
 *
 * Raw loaders only: file -> GaussianSplat. No asset import/cook pipeline.
 */
#pragma once

#include <string_view>

#include "vrf/asset/gaussian_splat.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    // Force a specific format regardless of the file extension.
    [[nodiscard]] Expected<void> LoadGaussianSplatPly(std::string_view path, GaussianSplat& out);
    [[nodiscard]] Expected<void> LoadGaussianSplatSplat(std::string_view path, GaussianSplat& out);

    // Dispatch by extension: .ply / .compressed.ply / .spz / .splat / .ksplat.
    [[nodiscard]] Expected<void> LoadGaussianSplat(std::string_view path, GaussianSplat& out);
} // namespace vrf
