#include <doctest/doctest.h>

#include <filesystem>
#include <vrf/asset/loaders/fbx_loader.hpp>

TEST_CASE("FBX loader reports unavailable or missing input without modifying output")
{
    vrf::Mesh mesh;
    mesh.name = "sentinel";
    CHECK_FALSE(vrf::LoadFbx("missing-fbx-fixture.fbx", mesh).has_value());
    CHECK(mesh.name == "sentinel");
}

#ifdef VRF_TEST_FBX
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
