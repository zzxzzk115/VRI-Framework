#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vrf/vrf.hpp>

TEST_CASE("FBX loader reports unavailable or missing input without modifying output")
{
    vrf::Mesh mesh;
    mesh.name = "sentinel";
    CHECK_FALSE(vrf::LoadFbx("missing-fbx-fixture.fbx", mesh).has_value());
    CHECK(mesh.name == "sentinel");
}

#ifdef VRF_TEST_FBX
TEST_CASE("FBX accepts small invertible transforms and rejects zero scale")
{
    std::ifstream input(std::filesystem::path(VRF_TEST_ASSET_DIR) / "fbx_static.fbx", std::ios::binary);
    REQUIRE(input.good());
    const std::string original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    for (const auto scale : {"1e-6, 1e-6, 1e-6", "-1e-6, 1e-6, 1e-6", "1e-8, 2e-6, 1e-4", "0, 1, 1"})
    {
        INFO(std::string(scale));
        std::string       text              = original;
        const std::string translation       = "\"Lcl Translation\", \"Lcl Translation\", \"\", \"A\", 100, 0, 0";
        const auto        translationOffset = text.find(translation);
        REQUIRE(translationOffset != std::string::npos);
        text.replace(
            translationOffset, translation.size(), "\"Lcl Translation\", \"Lcl Translation\", \"\", \"A\", 0, 0, 0");
        const std::string needle = "\"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\", -1, 2, 1";
        const auto        offset = text.find(needle);
        REQUIRE(offset != std::string::npos);
        text.replace(offset, needle.size(), std::string("\"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\", ") + scale);
        struct TemporaryFbx
        {
            std::filesystem::path path;
            ~TemporaryFbx()
            {
                std::error_code error;
                std::filesystem::remove(path, error);
            }
        } temporary {
            std::filesystem::temp_directory_path() /
            ("vrf-scale-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".fbx")};
        {
            std::ofstream output(temporary.path, std::ios::binary);
            output << text;
            REQUIRE(output.good());
        }
        vrf::Mesh mesh;
        mesh.name = "sentinel";
        vrf::FbxImportOptions options;
        options.loadTextures = false;
        const auto result    = vrf::LoadFbx(temporary.path.string(), mesh, options);
        if (scale[0] == '0')
        {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().message.find("singular mesh transform") != std::string::npos);
            CHECK(mesh.name == "sentinel");
            CHECK(mesh.positions.empty());
            continue;
        }
        CHECK(result.has_value());
        if (!result)
            continue;
        REQUIRE(mesh.positions.size() == 3);
        REQUIRE(mesh.indices.size() == 3);
        const auto a = mesh.positions[mesh.indices[0]];
        const auto b = mesh.positions[mesh.indices[1]];
        const auto c = mesh.positions[mesh.indices[2]];
        CHECK(glm::dot(glm::cross(b - a, c - a), mesh.normals[mesh.indices[0]]) > 0);
        for (const auto& normal : mesh.normals)
            CHECK(glm::length(normal) == doctest::Approx(1));
    }
}

TEST_CASE("FBX invalid geometry and texture paths preserve the Expected error contract")
{
    const auto    fixture = std::filesystem::path(VRF_TEST_ASSET_DIR) / "fbx_static.fbx";
    std::ifstream input(fixture, std::ios::binary);
    REQUIRE(input.good());
    const std::string original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    struct InvalidInput
    {
        std::string needle, replacement, error;
        bool        loadTextures    = false;
        bool        convertToMeters = true;
    };
    for (const auto& invalid :
         {InvalidInput {"Normals: *9 { a: 0,0,1,0,0,1,0,0,1 }",
                        "Normals: *9 { a: 0,0,1e309,0,0,1,0,0,1 }",
                        "invalid position or normal"},
          InvalidInput {"rgba8_2x2.dds", std::string(300, 'x') + ".dds", "texture", true},
          InvalidInput {"UV: *6 { a: 0,0,1,0,0,1 }", "UV: *6 { a: 1e309,0,1,0,0,1 }", "invalid texture coordinate"},
          InvalidInput {"UV: *6 { a: 0,0,1,0,0,1 }", "UV: *6 { a: 0,-1e309,1,0,0,1 }", "invalid texture coordinate"},
          InvalidInput {"\"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\", -1, 2, 1",
                        "\"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\", 1e38, 2, 1",
                        "invalid position",
                        false,
                        false},
          InvalidInput {"\"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\", -1, 2, 1",
                        "\"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\", -1e38, 2, 1",
                        "invalid position",
                        false,
                        false},
          InvalidInput {"\"UnitScaleFactor\", \"double\", \"Number\", \"\", 1",
                        "\"UnitScaleFactor\", \"double\", \"Number\", \"\", 3e38",
                        "invalid position"}})
    {
        std::string text        = original;
        const auto& needle      = invalid.needle;
        const auto& replacement = invalid.replacement;
        auto        offset      = text.find(needle);
        REQUIRE(offset != std::string::npos);
        do
        {
            text.replace(offset, needle.size(), replacement);
            offset = text.find(needle, offset + replacement.size());
        } while (offset != std::string::npos);
        struct TemporaryFbx
        {
            std::filesystem::path path;
            ~TemporaryFbx()
            {
                std::error_code error;
                std::filesystem::remove(path, error);
            }
        } temporary {
            std::filesystem::temp_directory_path() /
            ("vrf-invalid-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".fbx")};
        {
            std::ofstream output(temporary.path, std::ios::binary);
            output << text;
            REQUIRE(output.good());
        }
        vrf::Mesh mesh;
        mesh.name = "sentinel";
        vrf::FbxImportOptions options;
        options.loadTextures    = invalid.loadTextures;
        options.convertToMeters = invalid.convertToMeters;
        const auto load         = [&] {
            const auto result = vrf::LoadFbx(temporary.path.string(), mesh, options);
            INFO(invalid.replacement);
            INFO((result ? "success" : result.error().message));
            CHECK_FALSE(result.has_value());
            if (!result)
                CHECK(result.error().message.find(invalid.error) != std::string::npos);
        };
        CHECK_NOTHROW(load());
        CHECK(mesh.name == "sentinel");
        CHECK(mesh.positions.empty());
    }
}

TEST_CASE("FBX Phong transparency preserves opacity and alpha mode")
{
    for (const bool transparent : {false, true})
    {
        const auto path =
            (std::filesystem::path(VRF_TEST_ASSET_DIR) / (transparent ? "fbx_transparent.fbx" : "fbx_static.fbx"))
                .string();
        vrf::Mesh mesh;
        REQUIRE(vrf::LoadFbx(path, mesh).has_value());
        REQUIRE(mesh.materials.size() == 1);
        const auto& material = mesh.materials[0];
        const auto* phong    = std::get_if<vrf::PhongMaterial>(&material.core);
        REQUIRE(phong);
        CHECK(phong->opacity == doctest::Approx(transparent ? 0.25f : 1.0f));
        CHECK(phong->diffuse.a == doctest::Approx(phong->opacity));
        CHECK(material.alphaMode == (transparent ? vrf::AlphaMode::Blend : vrf::AlphaMode::Opaque));
    }
}

TEST_CASE("FBX static import bakes hierarchy, reflected winding, units and normal convention")
{
    const auto            path = (std::filesystem::path(VRF_TEST_ASSET_DIR) / "fbx_static.fbx").string();
    vrf::Mesh             mesh;
    vrf::FbxImportOptions options;
    options.materialConvention = vrf::FbxMaterialConvention::OrcaMetallicRoughness;
    const auto result          = vrf::LoadFbx(path, mesh, options);
    INFO((result ? "" : result.error().message));
    REQUIRE(result.has_value());
    REQUIRE(mesh.positions.size() == 3);
    REQUIRE(mesh.indices.size() == 3);
    CHECK(mesh.boundsMin.x == doctest::Approx(0));
    CHECK(mesh.boundsMax.x == doctest::Approx(1));
    CHECK(mesh.boundsMax.y == doctest::Approx(2));
    const auto a = mesh.positions[mesh.indices[0]];
    const auto b = mesh.positions[mesh.indices[1]];
    const auto c = mesh.positions[mesh.indices[2]];
    CHECK(glm::dot(glm::cross(b - a, c - a), mesh.normals[mesh.indices[0]]) > 0);
    REQUIRE(mesh.materials.size() == 1);
    const auto* material = std::get_if<vrf::PbrMetallicRoughnessMaterial>(&mesh.materials[0].core);
    REQUIRE(material);
    CHECK(material->baseColorFactor.r == doctest::Approx(0.2));
    CHECK(material->metallicFactor == 0);
    CHECK_FALSE(material->occlusionTexture.Valid());
    CHECK(mesh.materials[0].doubleSided);
    REQUIRE(mesh.textures.size() == 1);
    CHECK(mesh.textures[0].fileFormat == vrf::TextureFileFormat::DDS);
    CHECK(mesh.textures[0].width == 2);
    options.directXNormalMaps = true;
    vrf::Mesh directX;
    REQUIRE(vrf::LoadFbx(path, directX, options).has_value());
    for (size_t i = 0; i < mesh.tangents.size(); ++i)
    {
        CHECK(directX.tangents[i].w == -mesh.tangents[i].w);
        CHECK(directX.texCoords0[i] == mesh.texCoords0[i]);
        CHECK(glm::length(directX.normals[i]) == doctest::Approx(1));
        const auto glBitangent = glm::cross(mesh.normals[i], glm::vec3(mesh.tangents[i])) * mesh.tangents[i].w;
        const auto dxBitangent = glm::cross(directX.normals[i], glm::vec3(directX.tangents[i])) * directX.tangents[i].w;
        // Original source V points +Y. A DirectX map encodes a +Y tilt with
        // negative green; both conventions must shade that tilt toward +Y.
        CHECK(glBitangent.y == doctest::Approx(1));
        CHECK(dxBitangent.y == doctest::Approx(-1));
    }
}
#endif
