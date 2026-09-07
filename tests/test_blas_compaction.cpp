#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include <vrf/gpu/raytracing.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/upload.hpp>

TEST_CASE("BLAS compaction and resource enumeration with validation")
{
    for (const auto api : {vrf::GraphicsApi::Vulkan, vrf::GraphicsApi::D3D12})
    {
        auto result =
            vrf::RenderDevice::Create({.api = api, .validation = true, .enabledFeatures = VriFeature_RayTracing});
        if (!result)
        {
            MESSAGE("backend unavailable: " << static_cast<int>(api));
            continue;
        }
        auto device = std::move(*result);
        MESSAGE("backend: " << std::string(device.ApiName()));
        const auto& core = device.Core();
        if (!device.Desc()->hasRayTracing)
        {
            MESSAGE("ray tracing unsupported");
            continue;
        }
        const float   vertices[] = {0, -0.5f, 0, 0.5f, 0.5f, 0, -0.5f, 0.5f, 0};
        VriBufferDesc desc {};
        desc.size           = sizeof(vertices);
        desc.usage          = VriBufferUsage_AccelerationBuildInput;
        desc.memoryLocation = VriMemoryLocation_HostUpload;
        VriBuffer* buffer   = nullptr;
        REQUIRE(core.CreateBuffer(device.Handle(), &desc, &buffer) == VriResult_Success);
        std::memcpy(core.MapBuffer(buffer, 0, sizeof(vertices)), vertices, sizeof(vertices));
        core.UnmapBuffer(buffer);
        {
            VriAsTrianglesDesc geometry {};
            geometry.vertexBuffer = buffer;
            geometry.vertexCount  = 3;
            geometry.vertexStride = sizeof(float) * 3;
            geometry.vertexFormat = VriFormat_RGB32_SFLOAT;
            auto blas             = vrf::Blas::Create(device,
                                                      {geometry},
                                          VriAsGeometry_Opaque,
                                          VriAccelerationStructureBuild_PreferFastTrace |
                                              VriAccelerationStructureBuild_AllowCompaction);
            REQUIRE(blas.has_value());
            vrf::ImmediateSubmit(device, [&](VriCommandBuffer* cmd) { blas->CmdBuild(cmd); });
            REQUIRE(blas->Compact(device).has_value());
            CHECK(blas->DeviceAddress() != 0);
            uint32_t count = 0;
            REQUIRE(core.EnumerateObjects(device.Handle(), &count, nullptr) == VriResult_Success);
            CHECK(count > 0);
        }
        core.DestroyBuffer(buffer);
    }
}
