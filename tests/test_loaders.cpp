#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <vrf/asset/loaders/gaussian_splat_loader.hpp>
#include <vrf/asset/loaders/image_loader.hpp>
#include <vrf/asset/loaders/ktx_loader.hpp>
#include <vrf/asset/texture.hpp>
#include <vrf/core/log.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/upload.hpp>

// Loaders are exercised against REAL asset files committed under tests/assets/ (small,
// self-generated - no license issues, CI needs no external files). Gaussian formats,
// which are trivial to synthesize, are generated in code. GPU tests skip when no device.

namespace
{
    void AppendU32(std::vector<uint8_t>& out, uint32_t value)
    {
        out.push_back(static_cast<uint8_t>(value & 0xff));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    }
    void AppendF32(std::vector<uint8_t>& out, float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        AppendU32(out, bits);
    }
    std::filesystem::path WriteTempFile(const std::string& name, const std::vector<uint8_t>& bytes)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        std::ofstream               file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return path;
    }

    std::string AssetPath(const char* name) { return std::string(VRF_TEST_ASSET_DIR) + "/" + name; }
} // namespace

// ---- textures: real committed fixture files ----

TEST_CASE("image loader: PNG fixture (stb)")
{
    vrf::Texture texture;
    REQUIRE(vrf::LoadImage(AssetPath("rgba8_2x2.png"), texture).has_value());
    CHECK(texture.width == 2);
    CHECK(texture.height == 2);
    CHECK(texture.format == VriFormat_RGBA8_UNORM);
    CHECK_FALSE(texture.compressed);
    CHECK(texture.data.size() == 2 * 2 * 4);
}

TEST_CASE("DDS loader: uncompressed RGBA8 fixture")
{
    vrf::Texture texture;
    REQUIRE(vrf::LoadKtxDds(AssetPath("rgba8_2x2.dds"), texture).has_value());
    CHECK(texture.width == 2);
    CHECK(texture.height == 2);
    CHECK(texture.format == VriFormat_RGBA8_UNORM);
    CHECK_FALSE(texture.compressed);
    CHECK(texture.subresources.size() == 1);
}

TEST_CASE("DDS loader: BC3-compressed fixture")
{
    vrf::Texture texture;
    REQUIRE(vrf::LoadKtxDds(AssetPath("bc3_4x4.dds"), texture).has_value());
    CHECK(texture.width == 4);
    CHECK(texture.height == 4);
    CHECK(texture.format == VriFormat_BC3_UNORM);
    CHECK(texture.compressed);
    REQUIRE(texture.subresources.size() == 1);
    CHECK(texture.subresources[0].size == 16); // one 4x4 BC3 block
}

// ---- gaussian splats: synthesized in code ----

TEST_CASE("gaussian .splat loader round-trip (synthetic)")
{
    std::vector<uint8_t> splat;
    for (int i = 0; i < 3; ++i)
    {
        AppendF32(splat, static_cast<float>(i));
        AppendF32(splat, 0.0f);
        AppendF32(splat, 0.0f);
        AppendF32(splat, 1.0f);
        AppendF32(splat, 1.0f);
        AppendF32(splat, 1.0f);
        splat.insert(splat.end(), {255, 128, 0, 255});
        splat.insert(splat.end(), {128, 128, 128, 255});
    }
    const auto path = WriteTempFile("vrf_test.splat", splat);

    vrf::GaussianSplat out;
    REQUIRE(vrf::LoadGaussianSplatSplat(path.string(), out).has_value());
    CHECK(out.numPoints == 3);
    CHECK(out.splats[2].position.x == doctest::Approx(2.0f));
    CHECK(out.splats[0].scale.x == doctest::Approx(0.0f));
    CHECK(out.splats[0].opacity > 0.0f);
    std::filesystem::remove(path);
}

TEST_CASE("gaussian 3DGS PLY loader (synthetic)")
{
    std::vector<uint8_t> ply;
    const char*          header = "ply\n"
                                  "format binary_little_endian 1.0\n"
                                  "element vertex 2\n"
                                  "property float x\nproperty float y\nproperty float z\n"
                                  "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
                                  "property float opacity\n"
                                  "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
                                  "property float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\n"
                                  "end_header\n";
    ply.insert(ply.end(), header, header + std::strlen(header));
    const float v0[14] = {1, 2, 3, 0.1f, 0.2f, 0.3f, 0.5f, -1, -2, -3, 1, 0, 0, 0};
    const float v1[14] = {4, 5, 6, 0.4f, 0.5f, 0.6f, -0.5f, -0.5f, -0.5f, -0.5f, 0, 1, 0, 0};
    for (float f : v0)
        AppendF32(ply, f);
    for (float f : v1)
        AppendF32(ply, f);
    const auto path = WriteTempFile("vrf_test_3dgs.ply", ply);

    vrf::GaussianSplat out;
    REQUIRE(vrf::LoadGaussianSplatPly(path.string(), out).has_value());
    CHECK(out.numPoints == 2);
    CHECK(out.shDegree == 0);
    CHECK(out.splats[0].position.z == doctest::Approx(3.0f));
    CHECK(out.splats[0].shDC.y == doctest::Approx(0.2f));
    CHECK(out.splats[0].opacity == doctest::Approx(0.5f));
    CHECK(out.splats[0].rotation.w == doctest::Approx(1.0f)); // rot_0 (w) -> rotation.w
    std::filesystem::remove(path);
}

// ---- GPU upload: real compressed fixture + a synthetic multi-mip image ----

TEST_CASE("GPU upload: BC3 fixture + multi-mip (skips without a device)")
{
    auto device = vrf::RenderDevice::Create({});
    if (!device)
    {
        WARN("no GPU/Vulkan device available; skipping GPU upload test");
        return;
    }

    int errorCount = 0;
    vrf::SetLogSink([&errorCount](vrf::LogLevel level, std::string_view message) {
        if (level == vrf::LogLevel::Error)
        {
            ++errorCount;
            std::fprintf(stderr, "[validation] %.*s\n", static_cast<int>(message.size()), message.data());
        }
    });

    // (1) Real BC3-compressed DDS -> GPU (exercises the compressed upload path).
    {
        vrf::Texture bc;
        REQUIRE(vrf::LoadKtxDds(AssetPath("bc3_4x4.dds"), bc).has_value());
        errorCount  = 0;
        auto result = vrf::UploadTexture(*device, bc);
        REQUIRE(result.has_value());
        CHECK(result->texture != nullptr);
        CHECK(errorCount == 0);
        result->Destroy(*device);
    }

    // (2) Synthetic uncompressed RGBA8 with two mips (2x2 then 1x1) - the multi-mip path.
    {
        vrf::Texture tex;
        tex.width       = 2;
        tex.height      = 2;
        tex.format      = VriFormat_RGBA8_UNORM;
        tex.mipLevels   = 2;
        tex.arrayLayers = 1;
        tex.data.assign(2 * 2 * 4 + 1 * 1 * 4, 0x7f);
        tex.subresources.push_back(vrf::TextureSubresource {0, 0, 0, 16, 2, 2, 0});
        tex.subresources.push_back(vrf::TextureSubresource {1, 0, 16, 4, 1, 1, 0});
        errorCount  = 0;
        auto result = vrf::UploadTexture(*device, tex);
        REQUIRE(result.has_value());
        CHECK(errorCount == 0);
        result->Destroy(*device);
    }

    vrf::SetLogSink({});
}

// ---- optional: real gaussian / texture assets via env vars (local only) ----

TEST_CASE("real 3DGS PLY (via VRF_TEST_PLY)")
{
    const char* plyPath = std::getenv("VRF_TEST_PLY");
    if (!plyPath)
        return;
    vrf::GaussianSplat out;
    REQUIRE(vrf::LoadGaussianSplatPly(plyPath, out).has_value());
    CHECK(out.numPoints > 0);
}

TEST_CASE("real DDS/KTX (via VRF_TEST_KTXDDS)")
{
    const char* texturePath = std::getenv("VRF_TEST_KTXDDS");
    if (!texturePath)
        return;
    vrf::Texture texture;
    REQUIRE(vrf::LoadKtxDds(texturePath, texture).has_value());
    CHECK(texture.format != VriFormat_Unknown);
    CHECK_FALSE(texture.Empty());
}
