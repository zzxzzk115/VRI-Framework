#include "vrf/fg/resource_access.hpp"

#include <cassert>
#include <utility>

#include <glm/integer.hpp> // bitfield{Insert/Extract}

namespace vrf::fg
{
    namespace
    {
        constexpr auto kReservedBitsOffset = 0;
        constexpr auto kReservedBits       = 1;

        constexpr auto kAttachmentMarker    = 1u;
        constexpr auto kNonAttachmentMarker = 0u;

        //
        // Attachment (23 bits):
        //
        // |  1 bit   | 3 bits | 2 bits | 11 bits |  3 bits  |   3 bits   |
        // |   [0]    | [1..3] | [4..5] | [6..16] | [17..19] |  [20..22]  |
        // | reserved |  index | aspect |  layer  |   face   | clearValue |

        constexpr auto kAttachmentIndexBits = 3;
        constexpr auto kImageAspectBits     = 2;
        constexpr auto kLayerBits           = 11;
        constexpr auto kFaceBits            = 3;
        constexpr auto kClearValueBits      = 3;

        constexpr auto kAttachmentIndexOffset = kReservedBits;
        constexpr auto kImageAspectOffset     = kAttachmentIndexOffset + kAttachmentIndexBits;
        constexpr auto kLayerOffset           = kImageAspectOffset + kImageAspectBits;
        constexpr auto kFaceOffset            = kLayerOffset + kLayerBits;
        constexpr auto kClearOffset           = kFaceOffset + kFaceBits;

        //
        // Location (7 bits):
        //
        // | 2 bits | 5 bits  |
        // |  set   | binding |

        constexpr auto kLocationBits = 7;

        constexpr auto kSetIndexBits     = 2;
        constexpr auto kBindingIndexBits = 5;

        constexpr auto kSetIndexOffset     = 0;
        constexpr auto kBindingIndexOffset = kSetIndexOffset + kSetIndexBits;

        //
        // BindingInfo (14 bits):
        //
        // |  1 bit   |  7 bits  |    6 bits     |
        // | reserved | location | pipelineStage |

        constexpr auto kBindingInfoBits = 14;

        constexpr auto kPipelineStageBits = 6;

        constexpr auto kLocationOffset      = kReservedBits;
        constexpr auto kPipelineStageOffset = kLocationOffset + kLocationBits;

        //
        // TextureRead (17 bits):
        //
        // |   14 bits   |  2 bits  |  2 bits  |
        // | bindingInfo |   type   |  aspect  |

        constexpr auto kTypeBits = 2;

        constexpr auto kTypeOffset                   = kBindingInfoBits;
        constexpr auto kTextureReadImageAspectOffset = kTypeOffset + kTypeBits;

        [[nodiscard]] uint32_t encode(const Location& v)
        {
            uint32_t bits {0};
            bits = glm::bitfieldInsert(bits, v.set, kSetIndexOffset, kSetIndexBits);
            bits = glm::bitfieldInsert(bits, v.binding, kBindingIndexOffset, kBindingIndexBits);
            return bits;
        }
        [[nodiscard]] uint32_t encode(const BindingInfo& v)
        {
            uint32_t bits {0};
            bits = glm::bitfieldInsert(bits, kNonAttachmentMarker, kReservedBitsOffset, kReservedBits);
            bits = glm::bitfieldInsert(bits, encode(v.location), kLocationOffset, kLocationBits);
            bits = glm::bitfieldInsert(
                bits, static_cast<uint32_t>(v.pipelineStage), kPipelineStageOffset, kPipelineStageBits);
            return bits;
        }
    } // namespace

    Attachment::operator uint32_t() const
    {
        uint32_t bits {0};
        bits = glm::bitfieldInsert(bits, kAttachmentMarker, kReservedBitsOffset, kReservedBits);
        assert(imageAspect != ImageAspect::None);
        bits = glm::bitfieldInsert(bits, static_cast<uint32_t>(imageAspect), kImageAspectOffset, kImageAspectBits);
        bits = glm::bitfieldInsert(bits, index, kAttachmentIndexOffset, kAttachmentIndexBits);
        bits = glm::bitfieldInsert(bits, layer ? *layer + 1u : 0u, kLayerOffset, kLayerBits);
        bits = glm::bitfieldInsert(bits, face ? std::to_underlying(*face) + 1u : 0u, kFaceOffset, kFaceBits);
        bits = glm::bitfieldInsert(
            bits, clearValue ? static_cast<uint32_t>(*clearValue) + 1u : 0u, kClearOffset, kClearValueBits);
        return bits;
    }

    Attachment decodeAttachment(const uint32_t bits)
    {
        Attachment out;
        out.index = glm::bitfieldExtract(bits, kAttachmentIndexOffset, kAttachmentIndexBits);

        out.imageAspect = static_cast<ImageAspect>(glm::bitfieldExtract(bits, kImageAspectOffset, kImageAspectBits));
        assert(out.imageAspect != ImageAspect::None);

        // nullopt is encoded as '0'
        if (const auto temp = glm::bitfieldExtract(bits, kLayerOffset, kLayerBits); temp != 0)
        {
            out.layer = temp - 1;
        }
        if (const auto temp = glm::bitfieldExtract(bits, kFaceOffset, kFaceBits); temp != 0)
        {
            out.face = static_cast<CubeFace>(temp - 1);
        }
        if (const auto temp = glm::bitfieldExtract(bits, kClearOffset, kClearValueBits); temp != 0)
        {
            out.clearValue = static_cast<ClearValue>(temp - 1);
        }
        return out;
    }

    bool holdsAttachment(const uint32_t bits)
    {
        return glm::bitfieldExtract(bits, kReservedBitsOffset, kReservedBits) == kAttachmentMarker;
    }

    Location::operator uint32_t() const { return encode(*this); }

    Location decodeLocation(const uint32_t bits)
    {
        return {
            .set     = glm::bitfieldExtract(bits, kSetIndexOffset, kSetIndexBits),
            .binding = glm::bitfieldExtract(bits, kBindingIndexOffset, kBindingIndexBits),
        };
    }

    BindingInfo::operator uint32_t() const { return encode(*this); }

    BindingInfo decodeBindingInfo(const uint32_t bits)
    {
        return {
            .location = decodeLocation(glm::bitfieldExtract(bits, kLocationOffset, kLocationBits)),
            .pipelineStage =
                static_cast<PipelineStage>(glm::bitfieldExtract(bits, kPipelineStageOffset, kPipelineStageBits)),
        };
    }

    TextureRead::operator uint32_t() const
    {
        uint32_t bits {0};
        bits = glm::bitfieldInsert(bits, encode(binding), 0, kBindingInfoBits);
        bits = glm::bitfieldInsert(bits, static_cast<uint32_t>(type), kTypeOffset, kTypeBits);
        assert(imageAspect != ImageAspect::None);
        bits = glm::bitfieldInsert(
            bits, static_cast<uint32_t>(imageAspect), kTextureReadImageAspectOffset, kImageAspectBits);
        return bits;
    }

    TextureRead decodeTextureRead(const uint32_t bits)
    {
        return TextureRead {
            .binding = decodeBindingInfo(bits),
            .type    = static_cast<TextureRead::Type>(glm::bitfieldExtract(bits, kTypeOffset, kTypeBits)),
            .imageAspect =
                static_cast<ImageAspect>(glm::bitfieldExtract(bits, kTextureReadImageAspectOffset, kImageAspectBits)),
        };
    }

    ImageWrite::operator uint32_t() const
    {
        uint32_t bits {0};
        bits = glm::bitfieldInsert(bits, encode(binding), 0, kBindingInfoBits);
        bits = glm::bitfieldInsert(bits, static_cast<uint32_t>(imageAspect), kBindingInfoBits, kImageAspectBits);
        return bits;
    }

    ImageWrite decodeImageWrite(const uint32_t bits)
    {
        return ImageWrite {
            .binding     = decodeBindingInfo(bits),
            .imageAspect = static_cast<ImageAspect>(glm::bitfieldExtract(bits, kBindingInfoBits, kImageAspectBits)),
        };
    }

    VriPipelineStageFlags convert(const PipelineStage pipelineStage)
    {
        VriPipelineStageFlags mask = VriPipelineStage_None;
        if (static_cast<bool>(pipelineStage & PipelineStage::Transfer))
        {
            mask |= VriPipelineStage_Transfer;
        }
        if (static_cast<bool>(pipelineStage & PipelineStage::VertexShader))
        {
            mask |= VriPipelineStage_VertexShader;
        }
        if (static_cast<bool>(pipelineStage & PipelineStage::GeometryShader))
        {
            mask |= VriPipelineStage_GeometryShader;
        }
        if (static_cast<bool>(pipelineStage & PipelineStage::FragmentShader))
        {
            mask |= VriPipelineStage_FragmentShader;
        }
        if (static_cast<bool>(pipelineStage & PipelineStage::ComputeShader))
        {
            mask |= VriPipelineStage_ComputeShader;
        }
        if (static_cast<bool>(pipelineStage & PipelineStage::RayTracingShader))
        {
            mask |= VriPipelineStage_RayTracingShader;
        }
        return mask;
    }

    VriImageAspectFlags convert(const ImageAspect aspect)
    {
        switch (aspect)
        {
            case ImageAspect::Color:
                return VriImageAspect_Color;
            case ImageAspect::Depth:
                return VriImageAspect_Depth;
            case ImageAspect::Stencil:
                return VriImageAspect_Stencil;
            case ImageAspect::None:
                break;
        }
        return VriImageAspect_None;
    }
} // namespace vrf::fg
