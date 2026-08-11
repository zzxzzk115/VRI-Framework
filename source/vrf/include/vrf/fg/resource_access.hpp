/*
 * resource_access.hpp - bit-packed resource access declarations for framegraph
 * passes.
 *
 * A pass declares how it touches a resource by encoding one of these structs
 * into the 32-bit flags of FrameGraph::Builder::read()/write(). The encodings
 * (and their bit layout) mirror libvultra's framegraph so pass code ports
 * across with no changes to its read/write call sites.
 */
#pragma once

#include <cstdint>
#include <optional>

#include <vri/vri.h>

namespace vrf::fg
{
    // Shader/transfer stages a binding is visible to. Combinable.
    //
    // NOTE: the value is bit-packed into BindingInfo with kPipelineStageBits width
    // (resource_access.cpp). Adding a flag past the top one requires widening that
    // constant *and* kBindingInfoBits by the same amount.
    enum class PipelineStage : uint32_t
    {
        Transfer          = 1u << 0,
        VertexShader      = 1u << 1,
        GeometryShader    = 1u << 2,
        FragmentShader    = 1u << 3,
        ComputeShader     = 1u << 4,
        RayTracingShader  = 1u << 5,
        TessControlShader = 1u << 6,
        TessEvalShader    = 1u << 7,
        // For a buffer read as draw/dispatch arguments by CmdDraw*Indirect /
        // CmdDispatchIndirect - the consumer is the command processor, not a shader.
        DrawIndirect      = 1u << 8,
    };
    [[nodiscard]] constexpr PipelineStage operator|(PipelineStage a, PipelineStage b)
    {
        return static_cast<PipelineStage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    [[nodiscard]] constexpr PipelineStage operator&(PipelineStage a, PipelineStage b)
    {
        return static_cast<PipelineStage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    // Single-plane image aspect (not a mask - one aspect per access).
    enum class ImageAspect : uint32_t
    {
        None = 0,
        Color,
        Depth,
        Stencil,
    };

    enum class CubeFace : uint32_t
    {
        PositiveX = 0,
        NegativeX,
        PositiveY,
        NegativeY,
        PositiveZ,
        NegativeZ,
    };

    enum class ClearValue : uint32_t
    {
        Zero,
        One,

        OpaqueBlack,
        OpaqueWhite,
        TransparentBlack,
        TransparentWhite,

        FakeSky,

        UIntMax,
    };

    struct Attachment
    {
        uint32_t                  index {0};
        ImageAspect               imageAspect {ImageAspect::None};
        std::optional<uint32_t>   layer;
        std::optional<CubeFace>   face;
        std::optional<ClearValue> clearValue;

        [[nodiscard]] operator uint32_t() const;
    };
    [[nodiscard]] Attachment decodeAttachment(uint32_t bits);
    [[nodiscard]] bool       holdsAttachment(uint32_t bits);

    struct Location
    {
        uint32_t set {0};
        uint32_t binding {0};

        [[nodiscard]] operator uint32_t() const;
    };
    [[nodiscard]] Location decodeLocation(uint32_t bits);

    struct BindingInfo
    {
        Location      location;
        PipelineStage pipelineStage {PipelineStage::FragmentShader};

        [[nodiscard]] operator uint32_t() const;
    };
    [[nodiscard]] BindingInfo decodeBindingInfo(uint32_t bits);

    struct TextureRead
    {
        BindingInfo binding;

        enum class Type : uint32_t
        {
            CombinedImageSampler, // sampled view; the sampler is a separate binding under VRI
            SampledImage,
            StorageImage,
        };
        Type        type {Type::CombinedImageSampler};
        ImageAspect imageAspect {ImageAspect::Color};

        [[nodiscard]] operator uint32_t() const;
    };
    [[nodiscard]] TextureRead decodeTextureRead(uint32_t bits);

    struct ImageWrite
    {
        BindingInfo binding;
        ImageAspect imageAspect {ImageAspect::Color};

        [[nodiscard]] operator uint32_t() const;
    };
    [[nodiscard]] ImageWrite decodeImageWrite(uint32_t bits);

    [[nodiscard]] VriPipelineStageFlags convert(PipelineStage);
    [[nodiscard]] VriImageAspectFlags   convert(ImageAspect);
} // namespace vrf::fg
