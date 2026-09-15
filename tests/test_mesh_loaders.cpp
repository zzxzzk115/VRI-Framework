#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <vrf/asset/loaders/gltf_loader.hpp>
#include <vrf/asset/loaders/obj_loader.hpp>
#include <vrf/asset/loaders/image_loader.hpp>
#include <vrf/asset/asset_cache.hpp>

// Self-contained mesh-loader coverage: inputs are synthesized in code (a text OBJ and a
// glTF + external .bin), so CI needs no external asset files.

namespace
{
    std::filesystem::path WriteText(const std::string& name, const std::string& text)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        std::ofstream               file(path);
        file << text;
        return path;
    }
} // namespace

TEST_CASE("OBJ loader (synthetic triangle)")
{
    const std::string obj  = "v 0 0 0\n"
                             "v 1 0 0\n"
                             "v 0 1 0\n"
                             "vn 0 0 1\n"
                             "f 1//1 2//1 3//1\n";
    const auto        path = WriteText("vrf_test.obj", obj);

    vrf::Mesh mesh;
    REQUIRE(vrf::LoadObj(path.string(), mesh).has_value());
    CHECK(mesh.VertexCount() == 3);
    CHECK(mesh.IndexCount() == 3);
    CHECK(mesh.subMeshes.size() == 1);
    CHECK(vrf::HasAttribute(mesh.attributes, vrf::VertexAttribute::Position));
    CHECK(vrf::HasAttribute(mesh.attributes, vrf::VertexAttribute::Normal));
    CHECK(mesh.positions[1].x == doctest::Approx(1.0f));

    std::filesystem::remove(path);
}

TEST_CASE("glTF loader mesh accessors (synthetic + external .bin)")
{
    // Buffer: 3 VEC3 float positions (36 bytes) then 3 UNSIGNED_SHORT indices (6 bytes).
    std::vector<uint8_t> bin;
    auto                 appendF32 = [&bin](float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        for (int i = 0; i < 4; ++i)
            bin.push_back(static_cast<uint8_t>((bits >> (8 * i)) & 0xff));
    };
    auto appendU16 = [&bin](uint16_t v) {
        bin.push_back(static_cast<uint8_t>(v & 0xff));
        bin.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    };
    const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (float f : positions)
        appendF32(f);
    appendU16(0);
    appendU16(1);
    appendU16(2);

    const std::filesystem::path binPath = std::filesystem::temp_directory_path() / "vrf_test_mesh.bin";
    {
        std::ofstream file(binPath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    }

    const std::string gltf     = R"({
        "asset": { "version": "2.0" },
        "buffers": [ { "uri": "vrf_test_mesh.bin", "byteLength": 42 } ],
        "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
            { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
        ],
        "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0] },
            { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
        ],
        "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "mode": 4 } ] } ]
    })";
    const auto        gltfPath = WriteText("vrf_test_mesh.gltf", gltf);

    vrf::Mesh mesh;
    REQUIRE(vrf::LoadGltf(gltfPath.string(), mesh).has_value());
    CHECK(mesh.VertexCount() == 3);
    CHECK(mesh.IndexCount() == 3);
    CHECK(mesh.subMeshes.size() == 1);
    CHECK(mesh.positions[1].x == doctest::Approx(1.0f));
    CHECK(mesh.indices[2] == 2);

    std::filesystem::remove(binPath);
    std::filesystem::remove(gltfPath);
}

// Draco-compressed geometry isn't supported: the loader must fail cleanly (not crash on the
// huge no-bufferView accessors Draco leaves behind). GLB itself is supported; only Draco isn't.
TEST_CASE("glTF loader rejects Draco-required files (synthetic)")
{
    const std::string gltf = R"({
        "asset": { "version": "2.0" },
        "extensionsRequired": [ "KHR_draco_mesh_compression" ],
        "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "mode": 4 } ] } ]
    })";
    const auto        path = WriteText("vrf_test_draco.gltf", gltf);

    vrf::Mesh mesh;
    CHECK_FALSE(vrf::LoadGltf(path.string(), mesh).has_value());

    std::filesystem::remove(path);
}

TEST_CASE("glTF mip generation preserves base pixels and rounded box filter")
{
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "vrf_test_gltf_mips";
    fs::create_directories(dir);
    fs::copy_file(fs::path(VRF_TEST_ASSET_DIR) / "rgba8_2x2.png", dir / "image.png",
                  fs::copy_options::overwrite_existing);
    const auto source = dir / "mesh.gltf";
    {
        std::ofstream file(source);
        file << R"({"asset":{"version":"2.0"},"images":[{"uri":"image.png"}]})";
    }
    vrf::Texture original;
    REQUIRE(vrf::LoadImage((dir / "image.png").string(), original).has_value());
    vrf::Mesh mesh;
    REQUIRE(vrf::LoadGltf(source.string(), mesh).has_value());
    REQUIRE(mesh.textures.size() == 1);
    const auto& texture = mesh.textures[0];
    REQUIRE(texture.data.size() == 20);
    CHECK(texture.mipLevels == 2);
    REQUIRE(texture.subresources.size() == 2);
    CHECK(texture.subresources[1].offset == 16);
    for (size_t i = 0; i < 16; ++i)
        CHECK(texture.data[i] == original.data[i]);
    for (size_t c = 0; c < 4; ++c)
        CHECK(texture.data[16 + c] ==
              (original.data[c] + original.data[4 + c] + original.data[8 + c] + original.data[12 + c] + 2) / 4);

    const auto cache = source.string() + ".vrfcache";
    REQUIRE(vrf::WriteBakedMesh(cache, source.string(), mesh).has_value());
    mesh.name = "replacement";
    REQUIRE(vrf::WriteBakedMesh(cache, source.string(), mesh).has_value());
    REQUIRE(vrf::ReadBakedMesh(cache, source.string(), mesh).has_value());
    CHECK(mesh.name == "replacement");
    const auto cacheSize = fs::file_size(cache);
    vrf::GltfImportOptions options;
    options.loadTextures = false;
    REQUIRE(vrf::LoadModelCached(source.string(), mesh, options).has_value());
    CHECK(mesh.textures.empty());
    CHECK(fs::file_size(cache) == cacheSize);
    REQUIRE(vrf::ReadBakedMesh(cache, source.string(), mesh).has_value());
    CHECK(mesh.textures.size() == 1);

    fs::remove(cache);
    fs::remove(source);
    fs::remove(dir / "image.png");
    fs::remove(dir);
}

TEST_CASE("glTF geometry-only import does not decode image bytes")
{
    const auto source = WriteText("vrf_test_no_image_decode.gltf",
        R"({"asset":{"version":"2.0"},"images":[{"uri":"data:image/png;base64,YWJjZA=="}]})");
    vrf::Mesh mesh;
    vrf::GltfImportOptions options;
    options.loadTextures = false;
    CHECK(vrf::LoadGltf(source.string(), mesh, options).has_value());
    CHECK(mesh.textures.empty());
    CHECK_FALSE(vrf::LoadGltf(source.string(), mesh).has_value());
    std::filesystem::remove(source);
}
