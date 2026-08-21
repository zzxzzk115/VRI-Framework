#include "vrf/gpu/shader_library.hpp"

#include <fstream>
#include <unordered_map>

#include <vshadersystem/shader_id.hpp>
#include <vshadersystem/types.hpp>
#include <vshadersystem/variant_key.hpp>
#include <vshadersystem/vsh_format.hpp>

namespace vrf
{
    namespace
    {
        namespace vsh = vshadersystem;

        Expected<vsh::ShaderStage> ToVshStage(ShaderStage stage)
        {
            switch (stage)
            {
                case ShaderStage::Vertex:
                    return vsh::ShaderStage::eVert;
                case ShaderStage::Fragment:
                    return vsh::ShaderStage::eFrag;
                case ShaderStage::Geometry:
                    return vsh::ShaderStage::eGeom;
                // Needs vshadersystem >= v1.2.0; older cookers dropped hull/domain entry
                // points silently, so Resolve() would report "not found" for these.
                case ShaderStage::TessControl:
                    return vsh::ShaderStage::eHull;
                case ShaderStage::TessEval:
                    return vsh::ShaderStage::eDomain;
                case ShaderStage::Compute:
                    return vsh::ShaderStage::eComp;
                case ShaderStage::Task:
                    return vsh::ShaderStage::eTask;
                case ShaderStage::Mesh:
                    return vsh::ShaderStage::eMesh;
                case ShaderStage::RayGen:
                    return vsh::ShaderStage::eRgen;
                case ShaderStage::Miss:
                    return vsh::ShaderStage::eRmiss;
                case ShaderStage::ClosestHit:
                    return vsh::ShaderStage::eRchit;
                case ShaderStage::AnyHit:
                    return vsh::ShaderStage::eRahit;
                case ShaderStage::Intersection:
                    return vsh::ShaderStage::eRint;
            }
            return MakeError(VriResult_InvalidArgument, "ShaderLibrary: unknown shader stage");
        }

        // Combine a shader-id hash with a stage into a single map key.
        uint64_t ShaderStageKey(uint64_t shaderIdHash, vsh::ShaderStage stage)
        {
            return shaderIdHash ^ (static_cast<uint64_t>(stage) * 0x9E3779B97F4A7C15ull);
        }

        // vrf::ShaderReflection mirrors the cooker's layout 1:1 (shader_reflection.hpp explains why
        // it is a mirror and not a re-export). These assertions are that mirror's contract: a
        // vshadersystem release that renumbers either enum breaks the build here instead of
        // silently shifting every descriptor kind or stage bit in loaded reflection.
        static_assert(static_cast<uint8_t>(vsh::DescriptorKind::eUniformBuffer) ==
                      static_cast<uint8_t>(DescriptorKind::UniformBuffer));
        static_assert(static_cast<uint8_t>(vsh::DescriptorKind::eAccelerationStructure) ==
                      static_cast<uint8_t>(DescriptorKind::AccelerationStructure));
        static_assert(static_cast<uint8_t>(vsh::DescriptorKind::eUnknown) ==
                      static_cast<uint8_t>(DescriptorKind::Unknown));
        static_assert(static_cast<uint8_t>(vsh::ResourceAccess::eReadWrite) ==
                      static_cast<uint8_t>(ResourceAccess::ReadWrite));
        static_assert(static_cast<uint8_t>(vsh::TextureType::eUnknown) ==
                      static_cast<uint8_t>(ReflectedTextureType::Unknown));
        static_assert(vsh::eStageVert == static_cast<uint32_t>(ShaderStageMask::Vertex));
        static_assert(vsh::eStageComp == static_cast<uint32_t>(ShaderStageMask::Compute));
        static_assert(vsh::eStageRchit == static_cast<uint32_t>(ShaderStageMask::ClosestHit));
        static_assert(vsh::eStageDomain == static_cast<uint32_t>(ShaderStageMask::TessEval));

        ShaderReflection MirrorReflection(const vsh::ShaderReflection& src)
        {
            ShaderReflection out;
            out.descriptors.reserve(src.descriptors.size());
            for (const vsh::DescriptorBinding& d : src.descriptors)
                out.descriptors.push_back({.name         = d.name,
                                           .set          = d.set,
                                           .binding      = d.binding,
                                           .count        = d.count,
                                           .kind         = static_cast<DescriptorKind>(d.kind),
                                           .access       = static_cast<ResourceAccess>(d.access),
                                           .stageFlags   = static_cast<ShaderStageMask>(d.stageFlags),
                                           .runtimeSized = d.runtimeSized,
                                           .textureType  = static_cast<ReflectedTextureType>(d.textureType)});

            out.blocks.reserve(src.blocks.size());
            for (const vsh::BlockLayout& b : src.blocks)
            {
                ReflectedBlock block {.name         = b.name,
                                      .set          = b.set,
                                      .binding      = b.binding,
                                      .size         = b.size,
                                      .pushConstant = b.isPushConstant,
                                      .stageFlags   = static_cast<ShaderStageMask>(b.stageFlags),
                                      .members      = {}};
                block.members.reserve(b.members.size());
                for (const vsh::BlockMember& m : b.members)
                    block.members.push_back({.name = m.name, .offset = m.offset, .size = m.size});
                out.blocks.push_back(std::move(block));
            }

            out.hasLocalSize = src.hasLocalSize;
            out.localSize[0] = src.localSizeX;
            out.localSize[1] = src.localSizeY;
            out.localSize[2] = src.localSizeZ;
            return out;
        }
    } // namespace

    struct ShaderLibrary::Impl
    {
        // Decoded variants (SPIR-V/WGSL live here for the library's lifetime).
        std::vector<vsh::ShaderBinary> binaries;
        // Framework-side mirror of each entry's reflection, index-aligned with `binaries`.
        // Converted once at load so Resolve stays a pure lookup, and stored separately (rather
        // than handed out as vsh::ShaderReflection&) to keep vshadersystem inside this file.
        // One entry per variant, but the cooker writes the same (base-variant) table into all of
        // a shader's variants - see the contract on ResolvedShader::reflection.
        std::vector<ShaderReflection> reflections;
        // variantHash -> index into `binaries`.
        std::unordered_map<uint64_t, size_t> byVariantHash;
        // (shaderIdHash, stage) -> the shader's declared PERMUTE keyword names. Only permute
        // keywords participate in the variant hash, so runtime/spec keywords are excluded.
        std::unordered_map<uint64_t, std::vector<std::string>> permuteKeywords;
    };

    std::vector<ShaderKeyword> ShaderKeywordsForVertexLayout(const GpuVertexLayout& layout)
    {
        // Names must match the [VshKeyword(...)] declarations in the cooked shader. Position is
        // implied (every mesh has it) and is not a keyword.
        return {
            {"VTX_HAS_NORMAL", layout.Has(VertexAttribute::Normal) ? 1u : 0u},
            {"VTX_HAS_TANGENT", layout.Has(VertexAttribute::Tangent) ? 1u : 0u},
            {"VTX_HAS_UV0", layout.Has(VertexAttribute::TexCoord0) ? 1u : 0u},
            {"VTX_HAS_COLOR", layout.Has(VertexAttribute::Color) ? 1u : 0u},
        };
    }

    ShaderLibrary::ShaderLibrary()                                    = default;
    ShaderLibrary::~ShaderLibrary()                                   = default;
    ShaderLibrary::ShaderLibrary(ShaderLibrary&&) noexcept            = default;
    ShaderLibrary& ShaderLibrary::operator=(ShaderLibrary&&) noexcept = default;

    Expected<ShaderLibrary> ShaderLibrary::LoadFromMemory(const void* data, size_t size)
    {
        if (data == nullptr || size == 0)
            return MakeError(VriResult_InvalidArgument, "ShaderLibrary::LoadFromMemory: empty data");

        const auto*                bytes = static_cast<const uint8_t*>(data);
        const std::vector<uint8_t> buffer(bytes, bytes + size);

        vsh::Result<vsh::v1::Library> lib = vsh::v1::read_library(buffer);
        if (!lib.isOk())
            return MakeError(VriResult_Failure, "ShaderLibrary: read_library failed: " + lib.error().message);

        auto impl = std::make_unique<Impl>();
        // Reserved to the final size up front: Resolve hands out pointers INTO these vectors, so a
        // reallocation mid-load would dangle every previously handed-out reflection.
        impl->binaries.reserve(lib.value().entries.size());
        impl->reflections.reserve(lib.value().entries.size());
        for (const vsh::v1::LibraryEntry& entry : lib.value().entries)
        {
            vsh::Result<vsh::ShaderBinary> bin = vsh::v1::read_binary(entry.blob);
            if (!bin.isOk())
                return MakeError(VriResult_Failure, "ShaderLibrary: read_binary failed: " + bin.error().message);

            const size_t index = impl->binaries.size();
            impl->binaries.push_back(std::move(bin.value()));
            const vsh::ShaderBinary& stored = impl->binaries[index];
            impl->reflections.push_back(MirrorReflection(stored.reflection));
            impl->byVariantHash.emplace(entry.variantHash, index);

            const uint64_t key = ShaderStageKey(stored.shaderIdHash, entry.stage);
            if (impl->permuteKeywords.find(key) == impl->permuteKeywords.end())
            {
                std::vector<std::string> names;
                for (const vsh::KeywordDecl& kw : stored.keywords)
                    if (kw.dispatch == vsh::KeywordDispatch::ePermutation)
                        names.push_back(kw.name);
                impl->permuteKeywords.emplace(key, std::move(names));
            }
        }

        ShaderLibrary out;
        out.m_impl = std::move(impl);
        return out;
    }

    Expected<ShaderLibrary> ShaderLibrary::LoadFromFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return MakeError(VriResult_Failure, "ShaderLibrary::LoadFromFile: cannot open " + path);

        const auto size = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);
        std::vector<char> data(size);
        if (!file.read(data.data(), static_cast<std::streamsize>(size)))
            return MakeError(VriResult_Failure, "ShaderLibrary::LoadFromFile: read failed for " + path);

        return LoadFromMemory(data.data(), data.size());
    }

    Expected<ResolvedShader> ShaderLibrary::Resolve(std::string_view                  shaderId,
                                                    ShaderStage                       stage,
                                                    const std::vector<ShaderKeyword>& keywords) const
    {
        if (!m_impl)
            return MakeError(VriResult_Failure, "ShaderLibrary::Resolve: library not loaded");

        auto vshStage = ToVshStage(stage);
        if (!vshStage)
            return std::unexpected(vshStage.error());

        const uint64_t shaderIdHash = vsh::shader_id_hash(shaderId);
        const uint64_t key          = ShaderStageKey(shaderIdHash, *vshStage);

        const auto declaredIt = m_impl->permuteKeywords.find(key);
        if (declaredIt == m_impl->permuteKeywords.end())
            return MakeError(VriResult_Failure,
                             "ShaderLibrary::Resolve: no such shader/stage in library: " + std::string(shaderId));

        // Build the variant key from exactly the declared permute keywords (order-independent -
        // VariantKey::build sorts internally), pulling each value from the supplied set (default 0).
        vsh::VariantKey vk;
        vk.setShaderIdHash(shaderIdHash);
        vk.setStage(*vshStage);
        // Whether this resolves to the base variant falls out of the same loop: base means every
        // declared permute keyword came out 0. See ResolvedShader::isBaseVariant for why callers
        // need it.
        bool isBase = true;
        for (const std::string& name : declaredIt->second)
        {
            uint32_t value = 0;
            for (const ShaderKeyword& provided : keywords)
                if (provided.name == name)
                {
                    value = provided.value;
                    break;
                }
            isBase = isBase && value == 0;
            vk.set(name, value);
        }

        const uint64_t variantHash = vk.build();
        const auto     hit         = m_impl->byVariantHash.find(variantHash);
        if (hit == m_impl->byVariantHash.end())
            return MakeError(VriResult_Failure,
                             "ShaderLibrary::Resolve: no variant for the requested keyword set of " +
                                 std::string(shaderId));

        const vsh::ShaderBinary& bin = m_impl->binaries[hit->second];
        ResolvedShader           out;
        out.spirv         = bin.spirv.empty() ? nullptr : bin.spirv.data();
        out.spirvSize     = bin.spirv.size() * sizeof(uint32_t);
        out.wgsl          = bin.wgsl.empty() ? nullptr : bin.wgsl.data();
        out.wgslSize      = bin.wgsl.size();
        out.dxbc          = bin.dxbc.empty() ? nullptr : bin.dxbc.data();
        out.dxbcSize      = bin.dxbc.size();
        out.dxil          = bin.dxil.empty() ? nullptr : bin.dxil.data();
        out.dxilSize      = bin.dxil.size();
        out.entryPoint    = bin.entryPointName;
        out.reflection    = &m_impl->reflections[hit->second];
        out.isBaseVariant = isBase;
        return out;
    }
} // namespace vrf
