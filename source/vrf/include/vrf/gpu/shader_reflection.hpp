/*
 * shader_reflection.hpp - what a cooked shader declares about its own bindings.
 *
 * Every .vshlib carries the descriptor reflection the cooker extracted: name, set, binding, array
 * count, kind, access, stage mask and view dimension, plus the constant-block layouts and the
 * compute [numthreads]. ShaderLibrary deserializes all of it at load time and used to drop it on
 * the floor - Resolve() copied out bytecode only - so every consumer re-declared the same bindings
 * by hand alongside the shader. A mismatch between the two spellings is not a validation error: it
 * is a device hang on the first frame that binds the pipeline, which is why exposing the data the
 * loader already holds is worth a public type.
 *
 * Read the contract on ResolvedShader::reflection before building anything on this: with
 * vshadersystem v1.2.0 the table describes the BASE variant and is shared by every variant of the
 * shader, so for a keyword-gated shader it is a cross-check, not a source of truth.
 *
 * This is a MIRROR of the cooker's structures, not vshadersystem's own types. That library is
 * confined to shader_library.cpp (PImpl) so its headers - and the spirv-cross / glslang it drags
 * in - stay out of vrf's public API, and the mirror preserves that. It also deliberately does NOT
 * translate into VriDescriptorType: it reports what the SHADER declares, and deciding what that
 * means for a particular pipeline layout belongs to whoever builds the layout.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vrf
{
    enum class DescriptorKind : uint8_t
    {
        UniformBuffer,
        StorageBuffer,
        SampledImage,
        StorageImage,
        Sampler,
        // Vulkan's combined sampler. Slang/HLSL sources normally produce a separate
        // SampledImage + Sampler pair instead, so a consumer must handle both spellings.
        CombinedImageSampler,
        AccelerationStructure,
        Unknown,
    };

    enum class ResourceAccess : uint8_t
    {
        Unknown,
        ReadOnly,
        WriteOnly,
        ReadWrite,
    };

    // The sampled view dimension of an image descriptor; Unknown for everything else.
    enum class ReflectedTextureType : uint8_t
    {
        Tex2D,
        TexCube,
        Tex3D,
        Tex2DArray,
        Unknown,
    };

    // Which stages of the pipeline see a binding. The bit positions are the COOKER's, mirrored
    // here so a mask read out of a .vshlib needs no translation; shader_library.cpp static_asserts
    // each one against vshadersystem's enum, so a cooker renumbering breaks the build rather than
    // silently shifting every stage mask by one.
    enum class ShaderStageMask : uint32_t
    {
        None         = 0,
        Vertex       = 1u << 0,
        Fragment     = 1u << 1,
        Geometry     = 1u << 2,
        Compute      = 1u << 3,
        Task         = 1u << 4,
        Mesh         = 1u << 5,
        RayGen       = 1u << 6,
        Miss         = 1u << 7,
        ClosestHit   = 1u << 8,
        AnyHit       = 1u << 9,
        Intersection = 1u << 10,
        TessControl  = 1u << 11,
        TessEval     = 1u << 12,
    };

    [[nodiscard]] constexpr bool HasStage(ShaderStageMask mask, ShaderStageMask stage) noexcept
    {
        return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(stage)) != 0;
    }

    struct ReflectedDescriptor
    {
        std::string name;
        uint32_t    set {0};
        uint32_t    binding {0};
        uint32_t    count {1}; // array size; 1 for a scalar binding

        DescriptorKind       kind {DescriptorKind::Unknown};
        ResourceAccess       access {ResourceAccess::Unknown};
        ShaderStageMask      stageFlags {ShaderStageMask::None};
        bool                 runtimeSized {false};
        ReflectedTextureType textureType {ReflectedTextureType::Unknown};
    };

    struct ReflectedBlockMember
    {
        std::string name;
        uint32_t    offset {0};
        uint32_t    size {0};
    };

    // A constant block: either a uniform buffer or, with pushConstant set, a push-constant range.
    struct ReflectedBlock
    {
        std::string name;
        uint32_t    set {0};
        uint32_t    binding {0};
        uint32_t    size {0}; // bytes
        bool        pushConstant {false};

        ShaderStageMask                   stageFlags {ShaderStageMask::None};
        std::vector<ReflectedBlockMember> members;
    };

    struct ShaderReflection
    {
        std::vector<ReflectedDescriptor> descriptors;
        std::vector<ReflectedBlock>      blocks;

        bool     hasLocalSize {false};
        uint32_t localSize[3] {1, 1, 1};

        // Both return null when nothing matches. "Not present" means the BASE variant does not
        // declare it, which is not the same as "this variant does not use it" - see the contract
        // on ResolvedShader::reflection.
        [[nodiscard]] const ReflectedDescriptor* Find(std::string_view name) const
        {
            for (const ReflectedDescriptor& d : descriptors)
                if (d.name == name)
                    return &d;
            return nullptr;
        }

        [[nodiscard]] const ReflectedDescriptor* FindAt(uint32_t set, uint32_t binding) const
        {
            for (const ReflectedDescriptor& d : descriptors)
                if (d.set == set && d.binding == binding)
                    return &d;
            return nullptr;
        }
    };
} // namespace vrf
