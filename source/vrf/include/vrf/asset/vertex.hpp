/*
 * vertex.hpp - loader-agnostic vertex attribute vocabulary.
 *
 * Meshes store attributes as parallel (SoA) arrays; VertexAttribute is the bitmask
 * of which arrays a mesh actually carries.
 */
#pragma once

#include <cstdint>

#include "vrf/core/math.hpp"

namespace vrf
{
    enum class VertexAttribute : uint32_t
    {
        None      = 0,
        Position  = 1u << 0,
        Normal    = 1u << 1,
        Tangent   = 1u << 2,
        Color     = 1u << 3,
        TexCoord0 = 1u << 4,
        TexCoord1 = 1u << 5,
    };

    [[nodiscard]] constexpr VertexAttribute operator|(VertexAttribute a, VertexAttribute b) noexcept
    {
        return static_cast<VertexAttribute>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    [[nodiscard]] constexpr VertexAttribute operator&(VertexAttribute a, VertexAttribute b) noexcept
    {
        return static_cast<VertexAttribute>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    constexpr VertexAttribute& operator|=(VertexAttribute& a, VertexAttribute b) noexcept
    {
        a = a | b;
        return a;
    }

    [[nodiscard]] constexpr bool HasAttribute(VertexAttribute set, VertexAttribute flag) noexcept
    {
        return (set & flag) != VertexAttribute::None;
    }

    // Canonical CPU-side attribute element types (interleaving is the GPU-upload step's concern).
    using Position = Vec3;
    using Normal   = Vec3;
    using Tangent  = Vec4; // xyz + handedness in w
    using Color    = Vec4;
    using TexCoord = Vec2;
} // namespace vrf
