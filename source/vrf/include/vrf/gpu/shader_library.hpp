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
#include "vrf/gpu/shader_reflection.hpp"
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
        Geometry,
        // Tessellation control / evaluation (HLSL hull / domain). Cooking these needs a
        // vshadersystem that maps SLANG_STAGE_HULL/_DOMAIN; older cookers silently drop
        // such entry points, so Resolve() reports "not found" rather than misbehaving.
        TessControl,
        TessEval,
        Compute,
        Task,
        Mesh,
        RayGen,
        Miss,
        ClosestHit,
        AnyHit,
        Intersection,
    };

    // A resolved variant's bytecode. Pointers are owned by the ShaderLibrary and stay valid
    // for its lifetime; feed them into a ShaderVariants for the pipeline builder. Any target may
    // be null if the library wasn't cooked (or was stripped) for it.
    struct ResolvedShader
    {
        const void* spirv     = nullptr;
        size_t      spirvSize = 0; // bytes
        const void* wgsl      = nullptr;
        size_t      wgslSize  = 0; // bytes
        const void* dxbc      = nullptr;
        size_t      dxbcSize  = 0; // bytes (Direct3D 12, SM5.1)
        const void* dxil      = nullptr;
        size_t      dxilSize  = 0; // bytes (Direct3D 12, SM6.0)
        std::string entryPoint;    // pipeline entry-point name baked into the variant

        // The shader's declared bindings, owned by the library. Null only for a .vshlib cooked
        // before the reflection section existed.
        //
        // IT IS NOT THIS VARIANT'S TABLE. vshadersystem v1.2.0 reflects the BASE variant (every
        // permute keyword 0) and stores that same table on every variant of the shader, while the
        // bytecode is specialized per variant. So a binding a keyword ADDS is missing from the
        // reflection of the very variant that uses it, and a binding a keyword REMOVES is still
        // listed. Deriving a descriptor-set layout from this for a non-base variant therefore
        // truncates it, which is a first-frame device hang with no validation message. Use it to
        // CROSS-CHECK a hand-written layout for the base configuration; do not use it to build one
        // for a keyword-gated shader until the cooker emits reflection per variant.
        // (Pinned by tests/test_shader_reflection.cpp, which fails when that changes.)
        const ShaderReflection* reflection = nullptr;

        // True when every permute keyword this shader declares resolved to 0 - i.e. exactly the
        // variant `reflection` describes. Anything that cross-checks against the reflection must
        // gate on this: on a non-base variant the table both over-reports (a binding the variant
        // dropped) and under-reports (one a keyword added), so a check that ignores it produces
        // findings that are all false and trains the reader to ignore real ones.
        bool isBaseVariant = false;
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

        // Load a cooked .vshlib shipped as a runtime asset file.
        [[nodiscard]] static Expected<ShaderLibrary> LoadFromFile(const std::string& path);

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
