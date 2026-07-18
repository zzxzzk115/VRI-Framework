#include <doctest/doctest.h>

#include <cstdio>
#include <string_view>
#include <vector>

#include <vrf/core/log.hpp>
#include <vrf/gpu/descriptor_table.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/upload.hpp>

// Exercises the DescriptorTable capability against a real device (skips without one). Both local
// backends report hasBindless=false, so this drives the FIXED-array FALLBACK path end-to-end:
// create pool + set + minimal layout, populate with real texture views, Commit (UpdateDescriptorRanges),
// and confirm no validation errors. On a bindless-capable device the same calls take the runtime-array
// path (IsBindless()==true); the shader-indexed-render verification of that path needs such hardware.
namespace
{
    vrf::GpuTexture MakeSolid(vrf::RenderDevice& device, const uint8_t rgba[4])
    {
        vrf::Texture t;
        t.width  = 1;
        t.height = 1;
        t.format = VriFormat_RGBA8_UNORM;
        t.data.assign(rgba, rgba + 4);
        t.subresources.push_back({.offset = 0, .size = 4, .width = 1, .height = 1});
        auto up = vrf::UploadTexture(device, t);
        REQUIRE(up.has_value());
        return std::move(*up);
    }
} // namespace

TEST_CASE("DescriptorTable: fixed-fallback populate/commit (skips without a device)")
{
    auto device = vrf::RenderDevice::Create({});
    if (!device)
    {
        WARN("no GPU/Vulkan device available; skipping DescriptorTable test");
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

    constexpr uint8_t     kWhite[4] = {255, 255, 255, 255};
    constexpr uint8_t     kRed[4]   = {255, 0, 0, 255};
    constexpr uint8_t     kGreen[4] = {0, 255, 0, 255};
    const vrf::GpuTexture white     = MakeSolid(*device, kWhite);
    const vrf::GpuTexture red       = MakeSolid(*device, kRed);
    const vrf::GpuTexture green     = MakeSolid(*device, kGreen);

    errorCount = 0;
    auto table = vrf::DescriptorTable::Create(*device,
                                              {.type               = VriDescriptorType_Texture,
                                               .capacity           = 64,
                                               .registerSpace      = 0,
                                               .baseRegister       = 0,
                                               .stages             = VriShaderStage_Compute,
                                               .fallbackDescriptor = white.view.get()});
    REQUIRE(table.has_value());
    CHECK(table->Capacity() == 64);
    CHECK(table->DescriptorSet() != nullptr);
    CHECK(table->IsBindless() == (device->Desc()->hasBindless == VRI_TRUE));

    // Populate a few entries (rest fall back to white), then flush.
    CHECK(table->Add(red.view.get()) == 0);
    CHECK(table->Add(green.view.get()) == 1);
    table->Write(10, red.view.get());
    table->Commit();
    CHECK(table->Count() == 11);

    // A second commit with more entries must also validate clean (re-update path).
    table->Write(63, green.view.get());
    table->Commit();

    CHECK(errorCount == 0);
    vrf::SetLogSink({});
}
