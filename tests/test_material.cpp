#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <vrf/asset/loaders/gltf_loader.hpp>
#include <vrf/asset/material.hpp>

// The material model is OPEN: a fast-path variant for common models + a typed,
// lossless extension bag + a customShader escape hatch. New material types
// (subsurface, hair, fluid, future KHR extensions) need no C++ change.

TEST_CASE("material: fast-path core variant")
{
    vrf::Material m;
    m.core = vrf::PhongMaterial {};
    CHECK(m.Model() == vrf::MaterialModel::Phong);
    CHECK(m.As<vrf::PhongMaterial>() != nullptr);
    CHECK(m.As<vrf::PbrMetallicRoughnessMaterial>() == nullptr);
}

TEST_CASE("material: typed KHR extension + strongly-typed view")
{
    vrf::MaterialExtension clearcoat;
    clearcoat.Set("clearcoatFactor", 0.8f);
    clearcoat.Set("clearcoatRoughnessFactor", 0.2f);
    clearcoat.Set("clearcoatTexture", vrf::TextureRef {3});

    const vrf::Material m = vrf::MaterialBuilder {}
                                .SetName("car_paint")
                                .SetExtension(vrf::ext_name::Clearcoat, clearcoat)
                                .AddFeature(vrf::MaterialFeature::Clearcoat)
                                .Build();

    CHECK(m.HasFeature(vrf::MaterialFeature::Clearcoat));
    const auto cc = m.Clearcoat();
    REQUIRE(cc.has_value());
    CHECK(cc->factor == doctest::Approx(0.8f));
    CHECK(cc->roughnessFactor == doctest::Approx(0.2f));
    CHECK(cc->texture.index == 3);
    CHECK_FALSE(m.Transmission().has_value()); // absent extension -> nullopt
}

TEST_CASE("material: custom / exotic materials via escape hatch + open extension")
{
    // Hair has no standard model: carry it as a customShader id + a named extension bag.
    // The framework does not interpret it - the renderer does.
    vrf::Material m = vrf::MaterialBuilder {}.SetName("fur").SetCustomShader("hair_marschner").Build();
    CHECK(m.customShader == "hair_marschner");
    CHECK(m.HasFeature(vrf::MaterialFeature::Custom));

    vrf::MaterialExtension hair;
    hair.Set("melanin", 0.3f);
    hair.Set("roughness", 0.1f);
    hair.Set("tint", vrf::Vec3(0.9f, 0.8f, 0.7f));
    m.extensions["hair"] = hair;

    const vrf::MaterialExtension* ext = m.Extension("hair");
    REQUIRE(ext != nullptr);
    CHECK(ext->GetFloat("melanin") == doctest::Approx(0.3f));
    CHECK(ext->GetVec3("tint").y == doctest::Approx(0.8f));
    CHECK(ext->Has("roughness"));
    CHECK_FALSE(ext->Has("nonexistent"));
}

// Self-contained: a synthetic glTF (materials only, no external buffers) exercises the
// glTF loader's KHR extension parsing + typed views in CI - no external asset needed.
TEST_CASE("glTF loader parses KHR extensions (synthetic)")
{
    static const char* kGltf = R"({
        "asset": { "version": "2.0" },
        "extensionsUsed": [ "KHR_materials_clearcoat", "KHR_materials_sheen", "KHR_materials_transmission" ],
        "materials": [
            {
                "name": "test_mat",
                "pbrMetallicRoughness": { "baseColorFactor": [1.0, 0.0, 0.0, 1.0], "metallicFactor": 0.25, "roughnessFactor": 0.6 },
                "extensions": {
                    "KHR_materials_clearcoat": { "clearcoatFactor": 0.8, "clearcoatRoughnessFactor": 0.2 },
                    "KHR_materials_sheen": { "sheenColorFactor": [0.9, 0.8, 0.7], "sheenRoughnessFactor": 0.3 },
                    "KHR_materials_transmission": { "transmissionFactor": 0.5 }
                }
            }
        ]
    })";

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vrf_test_material.gltf";
    {
        std::ofstream file(path);
        file << kGltf;
    }

    vrf::Mesh mesh;
    REQUIRE(vrf::LoadGltf(path.string(), mesh).has_value());
    REQUIRE(mesh.materials.size() == 1);
    const vrf::Material& m = mesh.materials[0];

    // fast-path core parsed
    REQUIRE(m.As<vrf::PbrMetallicRoughnessMaterial>() != nullptr);
    CHECK(m.As<vrf::PbrMetallicRoughnessMaterial>()->metallicFactor == doctest::Approx(0.25f));

    // extensions captured + feature flags set + typed views work
    CHECK(m.HasFeature(vrf::MaterialFeature::Clearcoat));
    CHECK(m.HasFeature(vrf::MaterialFeature::Sheen));
    CHECK(m.HasFeature(vrf::MaterialFeature::Transmission));

    const auto clearcoat = m.Clearcoat();
    REQUIRE(clearcoat.has_value());
    CHECK(clearcoat->factor == doctest::Approx(0.8f));
    CHECK(clearcoat->roughnessFactor == doctest::Approx(0.2f));

    const auto sheen = m.Sheen();
    REQUIRE(sheen.has_value());
    CHECK(sheen->colorFactor.x == doctest::Approx(0.9f));
    CHECK(sheen->roughnessFactor == doctest::Approx(0.3f));

    const auto transmission = m.Transmission();
    REQUIRE(transmission.has_value());
    CHECK(transmission->factor == doctest::Approx(0.5f));

    std::filesystem::remove(path);
}

// Optional: verify the glTF loader parses KHR material extensions from a real asset.
TEST_CASE("real glTF KHR extensions (via VRF_TEST_GLTF)")
{
    const char* gltfPath = std::getenv("VRF_TEST_GLTF");
    if (!gltfPath)
        return;

    vrf::Mesh mesh;
    REQUIRE(vrf::LoadGltf(gltfPath, mesh).has_value());

    bool anyExtension = false;
    for (const vrf::Material& m : mesh.materials)
    {
        if (!m.extensions.empty())
            anyExtension = true;
        for (const auto& [name, ext] : m.extensions)
            std::fprintf(stderr,
                         "[test] material '%s': extension %s (%zu params)\n",
                         m.name.c_str(),
                         name.c_str(),
                         ext.params.size());
    }
    CHECK(anyExtension);
}
