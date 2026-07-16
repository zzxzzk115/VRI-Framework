#include "vrf/asset/material.hpp"

// Typed views over the well-known glTF KHR material extensions. Each reads the
// extension's params (stored under the glTF-spec property names by the loader) and
// returns nullopt when the extension is absent. The `extensions` bag stays the
// source of truth; these are just ergonomic accessors for the common cases.

namespace vrf
{
    std::optional<ClearcoatInfo> Material::Clearcoat() const
    {
        const MaterialExtension* ext = Extension(ext_name::Clearcoat);
        if (!ext)
            return std::nullopt;
        ClearcoatInfo info;
        info.factor           = ext->GetFloat("clearcoatFactor", 0.0f);
        info.roughnessFactor  = ext->GetFloat("clearcoatRoughnessFactor", 0.0f);
        info.texture          = ext->GetTexture("clearcoatTexture");
        info.roughnessTexture = ext->GetTexture("clearcoatRoughnessTexture");
        info.normalTexture    = ext->GetTexture("clearcoatNormalTexture");
        return info;
    }

    std::optional<TransmissionInfo> Material::Transmission() const
    {
        const MaterialExtension* ext = Extension(ext_name::Transmission);
        if (!ext)
            return std::nullopt;
        TransmissionInfo info;
        info.factor  = ext->GetFloat("transmissionFactor", 0.0f);
        info.texture = ext->GetTexture("transmissionTexture");
        return info;
    }

    std::optional<VolumeInfo> Material::Volume() const
    {
        const MaterialExtension* ext = Extension(ext_name::Volume);
        if (!ext)
            return std::nullopt;
        VolumeInfo info;
        info.thicknessFactor     = ext->GetFloat("thicknessFactor", 0.0f);
        info.attenuationDistance = ext->GetFloat("attenuationDistance", 0.0f);
        info.attenuationColor    = ext->GetVec3("attenuationColor", Vec3(1.0f, 1.0f, 1.0f));
        info.thicknessTexture    = ext->GetTexture("thicknessTexture");
        return info;
    }

    std::optional<SheenInfo> Material::Sheen() const
    {
        const MaterialExtension* ext = Extension(ext_name::Sheen);
        if (!ext)
            return std::nullopt;
        SheenInfo info;
        info.colorFactor      = ext->GetVec3("sheenColorFactor", Vec3(0.0f));
        info.roughnessFactor  = ext->GetFloat("sheenRoughnessFactor", 0.0f);
        info.colorTexture     = ext->GetTexture("sheenColorTexture");
        info.roughnessTexture = ext->GetTexture("sheenRoughnessTexture");
        return info;
    }

    std::optional<SpecularInfo> Material::Specular() const
    {
        const MaterialExtension* ext = Extension(ext_name::Specular);
        if (!ext)
            return std::nullopt;
        SpecularInfo info;
        info.factor       = ext->GetFloat("specularFactor", 1.0f);
        info.colorFactor  = ext->GetVec3("specularColorFactor", Vec3(1.0f, 1.0f, 1.0f));
        info.texture      = ext->GetTexture("specularTexture");
        info.colorTexture = ext->GetTexture("specularColorTexture");
        return info;
    }

    std::optional<IridescenceInfo> Material::Iridescence() const
    {
        const MaterialExtension* ext = Extension(ext_name::Iridescence);
        if (!ext)
            return std::nullopt;
        IridescenceInfo info;
        info.factor           = ext->GetFloat("iridescenceFactor", 0.0f);
        info.ior              = ext->GetFloat("iridescenceIor", 1.3f);
        info.thicknessMinimum = ext->GetFloat("iridescenceThicknessMinimum", 100.0f);
        info.thicknessMaximum = ext->GetFloat("iridescenceThicknessMaximum", 400.0f);
        info.texture          = ext->GetTexture("iridescenceTexture");
        info.thicknessTexture = ext->GetTexture("iridescenceThicknessTexture");
        return info;
    }

    std::optional<AnisotropyInfo> Material::Anisotropy() const
    {
        const MaterialExtension* ext = Extension(ext_name::Anisotropy);
        if (!ext)
            return std::nullopt;
        AnisotropyInfo info;
        info.strength = ext->GetFloat("anisotropyStrength", 0.0f);
        info.rotation = ext->GetFloat("anisotropyRotation", 0.0f);
        info.texture  = ext->GetTexture("anisotropyTexture");
        return info;
    }

    std::optional<float> Material::EmissiveStrength() const
    {
        const MaterialExtension* ext = Extension(ext_name::EmissiveStrength);
        if (!ext)
            return std::nullopt;
        return ext->GetFloat("emissiveStrength", 1.0f);
    }

    std::optional<float> Material::Ior() const
    {
        const MaterialExtension* ext = Extension(ext_name::Ior);
        if (!ext)
            return std::nullopt;
        return ext->GetFloat("ior", 1.5f);
    }
} // namespace vrf
