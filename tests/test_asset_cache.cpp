#include <doctest/doctest.h>

#include "asset/derived_cache.hpp"
#include "asset/texture_compress.hpp"
#include "bc7decomp.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
    namespace fs = std::filesystem;
    struct Fixture
    {
        fs::path root =
            fs::temp_directory_path() /
            ("vrf-derived-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Fixture()
        {
            fs::create_directories(root);
            // Uncompressed 5x7 RGBA TGA. Both odd edges and alpha survive BC7 baking.
            std::vector<uint8_t> image(18 + 5 * 7 * 4, 0);
            image[2]  = 2;
            image[12] = 5;
            image[14] = 7;
            image[16] = 32;
            image[17] = 0x28;
            for (size_t i = 18; i < image.size(); i += 4)
            {
                image[i]     = 32;
                image[i + 1] = 64;
                image[i + 2] = 192;
                image[i + 3] = 128;
            }
            std::ofstream png(root / "image.tga", std::ios::binary);
            png.write(reinterpret_cast<const char*>(image.data()), image.size());
            const float   vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
            std::ofstream bin(root / "mesh.bin", std::ios::binary);
            bin.write(reinterpret_cast<const char*>(vertices), sizeof(vertices));
            WriteModel();
        }
        void WriteModel(const std::string& material = "original")
        {
            std::ofstream(root / "scene.gltf") << R"({
                "asset":{"version":"2.0"},
                "buffers":[{"uri":"mesh.bin","byteLength":96}],
                "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},
                    {"buffer":0,"byteOffset":36,"byteLength":36},
                    {"buffer":0,"byteOffset":72,"byteLength":24}],
                "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                    {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
                    {"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"}],
                "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":0}]}],
                "images":[{"uri":"image.tga","name":"image"}], "textures":[{"source":0}],
                "materials":[{"name":")" << material
                                               << R"(","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}]
            })";
        }
        vrf::AssetCacheOptions Options(vrf::AssetCacheStats* stats = nullptr) const
        {
            vrf::AssetCacheOptions options;
            options.directory = (root / "cache").string();
            options.stats     = stats;
            return options;
        }
        std::vector<fs::path> Entries(const char* kind) const
        {
            std::vector<fs::path> paths;
            auto                  directory = root / "cache" / "derived-v1" / kind;
            if (fs::exists(directory))
                for (const auto& entry : fs::recursive_directory_iterator(directory))
                    if (entry.is_regular_file())
                        paths.push_back(entry.path());
            return paths;
        }
        ~Fixture()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };
} // namespace

TEST_CASE("derived cache reuses textures and only generated geometry across model paths")
{
    Fixture              f;
    vrf::AssetCacheStats stats;
    auto                 options = f.Options(&stats);
    vrf::Mesh            cold, warm;
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), cold, {}, options));
    CHECK(stats.textureMisses == 1);
    CHECK_FALSE(stats.tangentHit);
    CHECK(stats.bytesWritten > 0);
    REQUIRE(cold.tangents.size() == 3);
    REQUIRE(f.Entries("tangents").size() == 1);
    CHECK(fs::file_size(f.Entries("tangents")[0]) == 32 + 3 * sizeof(vrf::Tangent));
    CHECK(f.Entries("textures").size() == 1);
    f.WriteModel("changed material");
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), warm, {}, options));
    CHECK(stats.textureHits == 1);
    CHECK(stats.textureMisses == 0);
    CHECK(stats.tangentHit);
    CHECK(stats.bytesWritten == 0);
    CHECK(warm.positions == cold.positions);
    CHECK(warm.tangents == cold.tangents);
    CHECK(warm.textures[0].data == cold.textures[0].data);
    CHECK(warm.materials[0].name == "changed material");
    fs::create_directory(f.root / "copy");
    for (const auto* name : {"scene.gltf", "mesh.bin", "image.tga"})
        fs::copy_file(f.root / name, f.root / "copy" / name);
    REQUIRE(vrf::LoadModelCached((f.root / "copy/scene.gltf").string(), warm, {}, options));
    CHECK(stats.textureHits == 1);
    CHECK(stats.tangentHit);
    CHECK(stats.bytesWritten == 0);
    CHECK_FALSE(fs::exists(f.root / "scene.gltf.vrfcache"));
}

TEST_CASE("derived cache keys track source bytes and actual tangent inputs")
{
    Fixture              f;
    vrf::AssetCacheStats stats;
    auto                 options = f.Options(&stats);
    vrf::Mesh            mesh;
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    const auto stamp = fs::last_write_time(f.root / "image.tga");
    {
        std::fstream image(f.root / "image.tga", std::ios::binary | std::ios::in | std::ios::out);
        image.seekp(18);
        image.put(128);
    }
    fs::last_write_time(f.root / "image.tga", stamp);
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    CHECK(stats.textureMisses == 1);
    CHECK(stats.tangentHit);
    {
        std::fstream bin(f.root / "mesh.bin", std::ios::binary | std::ios::in | std::ios::out);
        const float  changed = -1;
        bin.seekp(80);
        bin.write(reinterpret_cast<const char*>(&changed), sizeof(changed));
    }
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    CHECK(stats.textureHits == 1);
    CHECK_FALSE(stats.tangentHit);
    CHECK(f.Entries("tangents").size() == 2);
}

TEST_CASE("derived cache corruption is rebuilt and write-disabled misses do not write")
{
    Fixture              f;
    vrf::AssetCacheStats stats;
    auto                 options = f.Options(&stats);
    options.write                = false;
    vrf::Mesh mesh;
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    CHECK_FALSE(fs::exists(f.root / "cache"));
    options.write = true;
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    auto texture = f.Entries("textures")[0];
    fs::resize_file(texture, 33);
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    CHECK(stats.textureMisses == 1);
    CHECK(stats.tangentHit);
    auto tangent = f.Entries("tangents")[0];
    {
        std::fstream file(tangent, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(40);
        file.put(99);
    }
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    CHECK(stats.textureHits == 1);
    CHECK_FALSE(stats.tangentHit);
    options.write = false;
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    CHECK(stats.tangentHit);
    CHECK(stats.bytesWritten == 0);
}

TEST_CASE("texture-disabled import skips decoding and a missing source still fails")
{
    Fixture   f;
    vrf::Mesh mesh;
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, f.Options()));
    std::ofstream(f.root / "image.tga") << "broken image";
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {.loadTextures = false}, f.Options()));
    CHECK(mesh.textures.empty());
    CHECK_FALSE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, f.Options()));
    fs::remove(f.root / "mesh.bin");
    CHECK_FALSE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {.loadTextures = false}, f.Options()));
}

TEST_CASE("derived cache handles concurrent writers and unavailable cache directories")
{
    Fixture   f;
    vrf::Mesh a, b;
    bool      successA = false, successB = false;
    {
        std::jthread first(
            [&] { successA = bool(vrf::LoadModelCached((f.root / "scene.gltf").string(), a, {}, f.Options())); });
        std::jthread second(
            [&] { successB = bool(vrf::LoadModelCached((f.root / "scene.gltf").string(), b, {}, f.Options())); });
    }
    REQUIRE(successA);
    REQUIRE(successB);
    CHECK(a.textures[0].data == b.textures[0].data);
    CHECK(f.Entries("textures").size() == 1);
    CHECK(f.Entries("tangents").size() == 1);
    auto options      = f.Options();
    options.directory = (f.root / "mesh.bin" / "impossible").string();
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), a, {}, options));
}

TEST_CASE("authored tangents are preserved and full-mesh caching remains opt-in")
{
    Fixture   f;
    vrf::Mesh mesh;
    REQUIRE(vrf::LoadGltf((f.root / "scene.gltf").string(), mesh));
    mesh.tangents.assign(mesh.positions.size(), vrf::Tangent(0, 1, 0, -1));
    vrf::detail::DerivedCache cache {f.Options()};
    cache.Tangents(mesh);
    CHECK(mesh.tangents[0] == vrf::Tangent(0, 1, 0, -1));
    CHECK(f.Entries("tangents").empty());

    auto options      = f.Options();
    options.mode      = vrf::AssetCacheMode::FullMesh;
    options.cachePath = (f.root / "whole.vrfcache").string();
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), mesh, {}, options));
    REQUIRE(fs::exists(options.cachePath));
    vrf::Mesh read;
    REQUIRE(vrf::ReadBakedMesh(options.cachePath, (f.root / "scene.gltf").string(), read));
    CHECK(read.tangents == mesh.tangents);
    REQUIRE(read.textures.size() == 1);
    CHECK(read.textures[0].data == mesh.textures[0].data);
    REQUIRE(vrf::LoadModelCached((f.root / "scene.gltf").string(), read, {.loadTextures = false}, options));
    CHECK(read.textures.empty());
}

#if defined(VRF_TEST_BC7)
TEST_CASE("direct BC7 honors mip offsets and padded input rows")
{
    vrf::Texture texture;
    texture.width     = 5;
    texture.height    = 7;
    texture.mipLevels = 3;
    texture.format    = VriFormat_RGBA8_UNORM;
    for (uint32_t mip = 0; mip < 3; ++mip)
    {
        vrf::TextureSubresource sub;
        sub.mipLevel      = mip;
        sub.width         = std::max(5u >> mip, 1u);
        sub.height        = std::max(7u >> mip, 1u);
        sub.offset        = texture.data.size() + 12;
        sub.rowPitchBytes = sub.width * 4 + 16;
        sub.size          = sub.rowPitchBytes * sub.height;
        texture.data.resize(sub.offset + sub.size, 0xCD);
        for (uint32_t y = 0; y < sub.height; ++y)
            for (uint32_t x = 0; x < sub.width; ++x)
            {
                uint8_t rgba[4] = {static_cast<uint8_t>(64 * mip), 128, 192, 255};
                std::memcpy(texture.data.data() + sub.offset + y * sub.rowPitchBytes + x * 4, rgba, 4);
            }
        texture.subresources.push_back(sub);
    }
    auto invalid = texture;
    invalid.subresources.pop_back();
    const auto before = invalid.data;
    CHECK_FALSE(vrf::detail::CompressTextureBc7(invalid));
    CHECK(invalid.data == before);
    REQUIRE(vrf::detail::CompressTextureBc7(texture));
    REQUIRE(texture.subresources.size() == 3);
    CHECK(texture.data.size() == 96);
    for (const auto& sub : texture.subresources)
    {
        CHECK(sub.rowPitchBytes == 0);
        bc7decomp::color_rgba pixels[16];
        REQUIRE(bc7decomp::unpack_bc7(texture.data.data() + sub.offset, pixels));
        for (const auto& pixel : pixels)
        {
            CHECK(std::abs(int(pixel.r) - int(sub.mipLevel) * 64) <= 3);
            // Portable mode 6 shares endpoint p-bits with RGB, allowing 254 for opaque input.
            CHECK(std::abs(int(pixel.a) - 255) <= 1);
        }
    }
}

TEST_CASE("direct BC7 pads odd edges, preserves alpha and encodes small mip tails")
{
    vrf::Texture source;
    source.width  = 5;
    source.height = 7;
    source.format = VriFormat_RGBA8_UNORM;
    source.data.resize(5 * 7 * 4);
    for (size_t i = 0; i < source.data.size(); i += 4)
    {
        source.data[i]     = 192;
        source.data[i + 1] = 64;
        source.data[i + 2] = 32;
        source.data[i + 3] = 128;
    }
    auto texture = source;
    REQUIRE(vrf::detail::CompressTextureBc7(texture));
    CHECK(texture.data.size() == 64);
    CHECK(texture.format == VriFormat_BC7_UNORM);
    for (size_t i = 0; i < texture.data.size(); i += 16)
    {
        bc7decomp::color_rgba pixels[16];
        REQUIRE(bc7decomp::unpack_bc7(texture.data.data() + i, pixels));
        for (const auto& pixel : pixels)
        {
            CHECK(std::abs(int(pixel.r) - 192) <= 3);
            CHECK(std::abs(int(pixel.g) - 64) <= 3);
            CHECK(std::abs(int(pixel.b) - 32) <= 3);
            CHECK(std::abs(int(pixel.a) - 128) <= 3);
        }
    }
    auto repeat = source;
    REQUIRE(vrf::detail::CompressTextureBc7(repeat));
    CHECK(repeat.data == texture.data);
    for (uint32_t size : {1, 2, 3})
    {
        auto small   = source;
        small.width  = size;
        small.height = size;
        small.data.resize(size * size * 4);
        REQUIRE(vrf::detail::CompressTextureBc7(small));
        CHECK(small.data.size() == 16);
    }
    auto malformed = source;
    malformed.data.pop_back();
    auto bytes = malformed.data;
    CHECK_FALSE(vrf::detail::CompressTextureBc7(malformed));
    CHECK(malformed.data == bytes);
}
#endif
