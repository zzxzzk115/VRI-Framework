/*
 * vertex_layout.hpp - compact, attribute-driven GPU vertex layout.
 *
 * A mesh only carries the attributes it actually has, so the framework does NOT
 * assume a fixed vertex struct. GpuVertexLayout describes the interleaved GPU vertex
 * for a given VertexAttribute mask: each present attribute, in a fixed canonical
 * order, gets a VriFormat + byte offset, and the whole thing a tight stride.
 *
 * The same mask drives (a) the interleaving in UploadMesh, (b) the pipeline's vertex
 * attributes, and (c) the shader variant keywords (VTX_HAS_*), so all three agree.
 * Because VRI matches vertex attributes to shader input locations POSITIONALLY (by the
 * order they are added, with no explicit location field), the canonical order here must
 * match the shader variant's compact input locations (position first, then each present
 * attribute in this order).
 */
#pragma once

#include <cstdint>
#include <vector>

#include <vri/vri.h>

#include "vrf/asset/vertex.hpp"

namespace vrf
{
    // One interleaved attribute: which attribute, its GPU format, and byte offset within a vertex.
    struct GpuVertexAttribute
    {
        VertexAttribute attribute;
        VriFormat       format;
        uint32_t        offset;
    };

    // The interleaved vertex layout for a VertexAttribute mask. `attributes` are in canonical
    // order (position, normal, tangent, color, texCoord0, texCoord1), skipping absent ones; the
    // Nth entry therefore corresponds to shader input location N.
    struct GpuVertexLayout
    {
        VertexAttribute                 mask   = VertexAttribute::Position;
        uint32_t                        stride = 0;
        std::vector<GpuVertexAttribute> attributes;

        [[nodiscard]] bool Has(VertexAttribute a) const noexcept { return HasAttribute(mask, a); }
    };

    namespace detail
    {
        // Canonical attribute order + GPU format + element size (bytes). Position is always first.
        struct VertexAttributeInfo
        {
            VertexAttribute attribute;
            VriFormat       format;
            uint32_t        size;
        };

        inline constexpr VertexAttributeInfo kVertexAttributeOrder[] = {
            {VertexAttribute::Position, VriFormat_RGB32_SFLOAT, 12},
            {VertexAttribute::Normal, VriFormat_RGB32_SFLOAT, 12},
            {VertexAttribute::Tangent, VriFormat_RGBA32_SFLOAT, 16}, // xyz + handedness (w)
            {VertexAttribute::Color, VriFormat_RGBA32_SFLOAT, 16},
            {VertexAttribute::TexCoord0, VriFormat_RG32_SFLOAT, 8},
            {VertexAttribute::TexCoord1, VriFormat_RG32_SFLOAT, 8},
        };
    } // namespace detail

    // Build a compact interleaved layout for `mask` (position is forced on - every mesh has it).
    [[nodiscard]] inline GpuVertexLayout MakeVertexLayout(VertexAttribute mask)
    {
        GpuVertexLayout layout;
        layout.mask     = mask | VertexAttribute::Position;
        uint32_t offset = 0;
        for (const detail::VertexAttributeInfo& info : detail::kVertexAttributeOrder)
        {
            if (!HasAttribute(layout.mask, info.attribute))
                continue;
            layout.attributes.push_back({info.attribute, info.format, offset});
            offset += info.size;
        }
        layout.stride = offset;
        return layout;
    }
} // namespace vrf
