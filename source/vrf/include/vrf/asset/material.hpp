/*
 * material.hpp - loader-agnostic material data with an OPEN shading model.
 *
 * A closed set of shading models can never be exhaustive (subsurface, hair, fluid,
 * the growing list of glTF KHR extensions, engine-custom BSDFs, node graphs...).
 * So Material is designed to be open, not enumerated:
 *
 *   1. `core`        - a fast-path std::variant over the COMMON models (Unlit /
 *                      PBR metallic-roughness / PBR spec-gloss / Phong).
 *   2. `extensions`  - an open, typed, lossless bag: name -> { typed params +
 *                      texture refs }. glTF KHR extensions, engine-custom, hair,
 *                      fluid, ... all live here. New material types need NO C++
 *                      change - just a new named extension.
 *   3. `features`    - MaterialFeature bitflags, a cheap routing/culling hint.
 *   4. `customShader`- escape hatch id for fully custom / node-graph materials.
 *
 * The framework only CARRIES this data - it does not interpret exotic shading
 * (that is the renderer's job; see the "shading is developer-owned" stance).
 * Strongly-typed accessors are provided for the well-known glTF KHR extensions so
 * common cases are ergonomic without closing the set.
 *
 * Texture references are indices into the owning Mesh's `textures` array.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>

#include "vrf/core/math.hpp"

namespace vrf
{
    enum class MaterialModel : uint8_t
    {
        Unlit,
        PbrMetallicRoughness,
        PbrSpecularGlossiness,
        Phong
    };

    enum class AlphaMode : uint8_t
    {
        Opaque,
        Mask,
        Blend
    };

    struct TextureRef
    {
        int index = -1;

        [[nodiscard]] constexpr bool Valid() const noexcept { return index >= 0; }
    };

    // ---- fast-path shading payloads --------------------------------------
    struct UnlitMaterial
    {
        Vec4       color = {1.0f, 1.0f, 1.0f, 1.0f};
        TextureRef colorTexture;
    };

    struct PbrMetallicRoughnessMaterial
    {
        Vec4  baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
        float metallicFactor  = 1.0f;
        float roughnessFactor = 1.0f;
        Vec3  emissiveFactor  = {0.0f, 0.0f, 0.0f};
        float ior             = 1.5f;

        TextureRef baseColorTexture;
        TextureRef metallicRoughnessTexture;
        TextureRef normalTexture;
        TextureRef occlusionTexture;
        TextureRef emissiveTexture;
    };

    struct PbrSpecularGlossinessMaterial
    {
        Vec4  diffuseFactor    = {1.0f, 1.0f, 1.0f, 1.0f};
        Vec3  specularFactor   = {1.0f, 1.0f, 1.0f};
        float glossinessFactor = 1.0f;

        TextureRef diffuseTexture;
        TextureRef specularGlossinessTexture;
        TextureRef normalTexture;
    };

    struct PhongMaterial
    {
        Vec4  diffuse   = {1.0f, 1.0f, 1.0f, 1.0f};
        Vec3  specular  = {0.0f, 0.0f, 0.0f};
        float shininess = 0.0f;
        Vec3  emissive  = {0.0f, 0.0f, 0.0f};
        float opacity   = 1.0f;
        float ior       = 1.5f;

        TextureRef diffuseTexture;
        TextureRef specularTexture;
        TextureRef normalTexture;
        TextureRef emissiveTexture;
    };

    using MaterialCore =
        std::variant<UnlitMaterial, PbrMetallicRoughnessMaterial, PbrSpecularGlossinessMaterial, PhongMaterial>;

    // ---- open, typed extension mechanism ---------------------------------
    // A single typed material parameter value.
    using MaterialParam = std::variant<float, int32_t, bool, Vec2, Vec3, Vec4, TextureRef, std::string>;

    // A named extension = a bag of typed params (lossless capture of anything the
    // fast-path core doesn't model).
    struct MaterialExtension
    {
        std::unordered_map<std::string, MaterialParam> params;

        [[nodiscard]] const MaterialParam* Find(std::string_view key) const
        {
            const auto it = params.find(std::string(key));
            return it == params.end() ? nullptr : &it->second;
        }
        [[nodiscard]] bool Has(std::string_view key) const { return Find(key) != nullptr; }

        [[nodiscard]] float GetFloat(std::string_view key, float fallback = 0.0f) const
        {
            if (const MaterialParam* p = Find(key))
            {
                if (const auto* f = std::get_if<float>(p))
                    return *f;
                if (const auto* i = std::get_if<int32_t>(p))
                    return static_cast<float>(*i);
            }
            return fallback;
        }
        [[nodiscard]] Vec3 GetVec3(std::string_view key, Vec3 fallback = Vec3(0.0f)) const
        {
            if (const MaterialParam* p = Find(key))
                if (const auto* v = std::get_if<Vec3>(p))
                    return *v;
            return fallback;
        }
        [[nodiscard]] Vec4 GetVec4(std::string_view key, Vec4 fallback = Vec4(0.0f)) const
        {
            if (const MaterialParam* p = Find(key))
                if (const auto* v = std::get_if<Vec4>(p))
                    return *v;
            return fallback;
        }
        [[nodiscard]] TextureRef GetTexture(std::string_view key) const
        {
            if (const MaterialParam* p = Find(key))
                if (const auto* t = std::get_if<TextureRef>(p))
                    return *t;
            return {};
        }
        void Set(std::string key, MaterialParam value) { params.insert_or_assign(std::move(key), std::move(value)); }
    };

    // Cheap routing/culling hints (the source of truth is `extensions`).
    enum class MaterialFeature : uint32_t
    {
        None             = 0,
        Clearcoat        = 1u << 0,
        Transmission     = 1u << 1,
        Volume           = 1u << 2, // volume / thin-walled -> subsurface look
        Sheen            = 1u << 3,
        Specular         = 1u << 4,
        Ior              = 1u << 5,
        Iridescence      = 1u << 6,
        Anisotropy       = 1u << 7,
        EmissiveStrength = 1u << 8,
        Subsurface       = 1u << 9,  // non-glTF SSS
        Hair             = 1u << 10, // hair BSDF
        Custom           = 1u << 31, // customShader / engine-specific
    };

    // Well-known glTF KHR extension names - use these as `extensions` keys.
    namespace ext_name
    {
        inline constexpr const char* Clearcoat        = "KHR_materials_clearcoat";
        inline constexpr const char* Transmission     = "KHR_materials_transmission";
        inline constexpr const char* Volume           = "KHR_materials_volume";
        inline constexpr const char* Sheen            = "KHR_materials_sheen";
        inline constexpr const char* Specular         = "KHR_materials_specular";
        inline constexpr const char* Ior              = "KHR_materials_ior";
        inline constexpr const char* Iridescence      = "KHR_materials_iridescence";
        inline constexpr const char* Anisotropy       = "KHR_materials_anisotropy";
        inline constexpr const char* EmissiveStrength = "KHR_materials_emissive_strength";
    } // namespace ext_name

    // Strongly-typed views over the well-known extensions (empty if the extension is absent).
    struct ClearcoatInfo
    {
        float      factor          = 0.0f;
        float      roughnessFactor = 0.0f;
        TextureRef texture;
        TextureRef roughnessTexture;
        TextureRef normalTexture;
    };
    struct TransmissionInfo
    {
        float      factor = 0.0f;
        TextureRef texture;
    };
    struct VolumeInfo
    {
        float      thicknessFactor     = 0.0f;
        float      attenuationDistance = 0.0f; // 0 == infinite
        Vec3       attenuationColor    = {1.0f, 1.0f, 1.0f};
        TextureRef thicknessTexture;
    };
    struct SheenInfo
    {
        Vec3       colorFactor     = {0.0f, 0.0f, 0.0f};
        float      roughnessFactor = 0.0f;
        TextureRef colorTexture;
        TextureRef roughnessTexture;
    };
    struct SpecularInfo
    {
        float      factor      = 1.0f;
        Vec3       colorFactor = {1.0f, 1.0f, 1.0f};
        TextureRef texture;
        TextureRef colorTexture;
    };
    struct IridescenceInfo
    {
        float      factor           = 0.0f;
        float      ior              = 1.3f;
        float      thicknessMinimum = 100.0f;
        float      thicknessMaximum = 400.0f;
        TextureRef texture;
        TextureRef thicknessTexture;
    };
    struct AnisotropyInfo
    {
        float      strength = 0.0f;
        float      rotation = 0.0f;
        TextureRef texture;
    };

    struct Material
    {
        std::string name;

        AlphaMode alphaMode   = AlphaMode::Opaque;
        float     alphaCutoff = 0.5f;
        bool      doubleSided = false;

        MaterialCore core = PbrMetallicRoughnessMaterial {};

        uint32_t features = 0; // MaterialFeature bitmask hint

        // Open, typed, lossless extensions (KHR / engine-custom / hair / fluid / ...).
        std::unordered_map<std::string, MaterialExtension> extensions;

        // Escape hatch: id of a fully custom / node-graph material the framework doesn't model.
        std::string customShader;

        [[nodiscard]] MaterialModel Model() const
        {
            return std::visit(
                [](auto&& payload) -> MaterialModel {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, UnlitMaterial>)
                        return MaterialModel::Unlit;
                    else if constexpr (std::is_same_v<T, PbrMetallicRoughnessMaterial>)
                        return MaterialModel::PbrMetallicRoughness;
                    else if constexpr (std::is_same_v<T, PbrSpecularGlossinessMaterial>)
                        return MaterialModel::PbrSpecularGlossiness;
                    else
                        return MaterialModel::Phong;
                },
                core);
        }

        template<class T>
        [[nodiscard]] const T* As() const noexcept
        {
            return std::get_if<T>(&core);
        }
        template<class T>
        [[nodiscard]] T* As() noexcept
        {
            return std::get_if<T>(&core);
        }

        [[nodiscard]] bool HasFeature(MaterialFeature feature) const noexcept
        {
            return (features & static_cast<uint32_t>(feature)) != 0;
        }
        void AddFeature(MaterialFeature feature) noexcept { features |= static_cast<uint32_t>(feature); }

        [[nodiscard]] const MaterialExtension* Extension(std::string_view name) const
        {
            const auto it = extensions.find(std::string(name));
            return it == extensions.end() ? nullptr : &it->second;
        }
        MaterialExtension& GetOrAddExtension(std::string name) { return extensions[std::move(name)]; }

        // Typed views over the well-known glTF KHR extensions (nullopt if absent).
        [[nodiscard]] std::optional<ClearcoatInfo>    Clearcoat() const;
        [[nodiscard]] std::optional<TransmissionInfo> Transmission() const;
        [[nodiscard]] std::optional<VolumeInfo>       Volume() const;
        [[nodiscard]] std::optional<SheenInfo>        Sheen() const;
        [[nodiscard]] std::optional<SpecularInfo>     Specular() const;
        [[nodiscard]] std::optional<IridescenceInfo>  Iridescence() const;
        [[nodiscard]] std::optional<AnisotropyInfo>   Anisotropy() const;
        [[nodiscard]] std::optional<float>            EmissiveStrength() const;
        // Dielectric index of refraction (KHR_materials_ior). nullopt if the extension is absent;
        // glTF/OpenPBR default is 1.5 (F0 = 0.04) when present without a value.
        [[nodiscard]] std::optional<float> Ior() const;
    };

    // Fluent builder for hand-authoring a Material (loaders fill Material directly).
    class MaterialBuilder
    {
    public:
        MaterialBuilder& SetName(std::string name)
        {
            m_material.name = std::move(name);
            return *this;
        }
        MaterialBuilder& SetAlphaMode(AlphaMode mode, float cutoff = 0.5f)
        {
            m_material.alphaMode   = mode;
            m_material.alphaCutoff = cutoff;
            return *this;
        }
        MaterialBuilder& SetDoubleSided(bool doubleSided)
        {
            m_material.doubleSided = doubleSided;
            return *this;
        }
        MaterialBuilder& SetCore(MaterialCore core)
        {
            m_material.core = std::move(core);
            return *this;
        }
        MaterialBuilder& AddFeature(MaterialFeature feature)
        {
            m_material.AddFeature(feature);
            return *this;
        }
        MaterialBuilder& SetExtension(std::string name, MaterialExtension extension)
        {
            m_material.extensions.insert_or_assign(std::move(name), std::move(extension));
            return *this;
        }
        MaterialBuilder& SetCustomShader(std::string shaderId)
        {
            m_material.customShader = std::move(shaderId);
            m_material.AddFeature(MaterialFeature::Custom);
            return *this;
        }

        [[nodiscard]] Material Build() const { return m_material; }

    private:
        Material m_material;
    };
} // namespace vrf
