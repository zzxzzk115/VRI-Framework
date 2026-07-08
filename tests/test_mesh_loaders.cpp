#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <vrf/asset/loaders/gltf_loader.hpp>
#include <vrf/asset/loaders/obj_loader.hpp>

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
