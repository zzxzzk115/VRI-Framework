/*
 * shader_library.hpp - runtime shader-variant selection over a cooked .vshlib.
 *
 * The framework does NOT assume a single monolithic shader. A .vshlib (produced offline by
 * vshadersystem's `vshaderc build`, all keyword permutations baked in) is loaded here; a
 * variant is then resolved at draw-setup time by (shaderId, stage, keyword values). The
 * keyword set is typically derived from a mesh's vertex layout (ShaderKeywordsForVertexLayout
 * -> VTX_HAS_NORMAL / VTX_HAS_TANGENT / ...), so a mesh without normals selects the variant
 * whose shader has no normal input.
 *
 * vshadersystem is confined to the .cpp (PImpl) - the runtime loader links spirv-cross /
 * glslang / xxhash, none of which belong in the public API. A resolved variant hands back
 * SPIR-V and (optionally) WGSL, ready to feed GraphicsPipelineBuilder::AddShaderVariants.
 * Note: vshadersystem binaries carry SPIR-V (+ optional WGSL) only - no DXBC/DXIL - so the
 * variant path targets Vulkan / OpenGL / Metal / WebGPU (D3D12 would need a separate blob).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vrf/core/result.hpp"
#include "vrf/gpu/vertex_layout.hpp"

namespace vrf
{
    // A permutation keyword and its value (bool keywords use 0/1). Names must match the
    // [VshKeyword(...)] declarations in the cooked shader (e.g. "VTX_HAS_NORMAL").
    struct ShaderKeyword
    {
        std::string name;
        uint32_t    value = 0;
    };

    enum class ShaderStage
    {
        Vertex,
        Fragment,
        Compute,
    };

    // A resolved variant's bytecode. Pointers are owned by the ShaderLibrary and stay valid
    // for its lifetime; feed them into a ShaderVariants for the pipeline builder.
    struct ResolvedShader
    {
        const void* spirv     = nullptr;
        size_t      spirvSize = 0; // bytes
        const void* wgsl      = nullptr;
        size_t      wgslSize  = 0; // bytes (0 if the library was cooked with --no-wgsl)
        std::string entryPoint;    // pipeline entry-point name baked into the variant
    };

    // The keywords the framework derives from a vertex layout. These MUST match the keyword
    // set the shader declares (the .vshlib only baked permutations of the declared keywords);
    // ShaderLibrary::Resolve additionally intersects against each shader's declared set, so a
    // shader may declare a subset without a hash mismatch.
    [[nodiscard]] std::vector<ShaderKeyword> ShaderKeywordsForVertexLayout(const GpuVertexLayout& layout);

    class ShaderLibrary
    {
    public:
        ShaderLibrary();
        ~ShaderLibrary();
        ShaderLibrary(ShaderLibrary&&) noexcept;
        ShaderLibrary& operator=(ShaderLibrary&&) noexcept;
        ShaderLibrary(const ShaderLibrary&)            = delete;
        ShaderLibrary& operator=(const ShaderLibrary&) = delete;

        // Load a cooked .vshlib from memory (e.g. an embedded byte array). All variants are
        // decoded up front, so Resolve is a pure lookup afterwards.
        [[nodiscard]] static Expected<ShaderLibrary> LoadFromMemory(const void* data, size_t size);

        // Resolve one stage of a shader for the given keyword values. `shaderId` is the path the
        // shader was cooked under, relative to --shader_root, without the .slang extension and with
        // forward slashes (e.g. "mesh"). Only keywords the shader declares are used; missing ones
        // default to 0.
        [[nodiscard]] Expected<ResolvedShader>
        Resolve(std::string_view shaderId, ShaderStage stage, const std::vector<ShaderKeyword>& keywords) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace vrf
