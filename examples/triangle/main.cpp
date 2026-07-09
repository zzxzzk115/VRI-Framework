// vrf triangle - the minimal VRI-Framework program: an Application (window +
// device + swapchain + present loop) plus a bufferless 3-vertex draw whose
// pipeline is assembled through the framework's builders.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vrf/vrf.hpp>

// Slang-authored triangle compiled offline to SPIR-V (one module, two entry
// points: vertexMain + fragmentMain). Vendored from VRI's shader library; the
// Vulkan backend consumes SPIR-V directly.
#include "shaders/triangle_spv.h"

int main()
{
    vrf::ApplicationDesc desc;
    desc.title  = "vrf triangle";
    desc.extent = {960, 540};
    // Request every optional feature best-effort. The triangle doesn't use them, but it makes the
    // startup capability dump (vrf::DescribeDevice) report what the GPU/backend actually supports.
    desc.enabledFeatures = VriFeature_RayTracing | VriFeature_RayQuery | VriFeature_MeshShader | VriFeature_Bindless |
                           VriFeature_VariableShadingRate | VriFeature_OpacityMicromap | VriFeature_ExternalMemory |
                           VriFeature_LowLatency;

    // VRF_WINDOW=glfw|sdl3 overrides the window backend (default Auto -> SDL3).
    if (const char* backend = std::getenv("VRF_WINDOW"))
    {
        if (std::strcmp(backend, "glfw") == 0)
            desc.windowBackend = vrf::WindowBackend::GLFW;
        else if (std::strcmp(backend, "sdl3") == 0)
            desc.windowBackend = vrf::WindowBackend::SDL3;
    }

    auto appResult = vrf::Application::Create(desc);
    if (!appResult)
    {
        std::fprintf(stderr, "[vrf-triangle] %s\n", appResult.error().message.c_str());
        return 1;
    }
    vrf::Application& app = *appResult.value();
    app.SetClearColor(0.1f, 0.1f, 0.12f);

    vrf::RenderDevice& device = app.Device();

    // Bufferless triangle: no descriptors, so an empty pipeline layout.
    auto layoutResult = vrf::PipelineLayoutBuilder {}.Build(device);
    if (!layoutResult)
    {
        std::fprintf(stderr, "[vrf-triangle] %s\n", layoutResult.error().message.c_str());
        return 1;
    }
    VriPipelineLayout* layout = *layoutResult;

    auto pipelineResult = vrf::GraphicsPipelineBuilder {}
                              .SetPipelineLayout(layout)
                              .AddShader(VriShaderStage_Vertex, g_triangleSpv, sizeof(g_triangleSpv), "vertexMain")
                              .AddShader(VriShaderStage_Fragment, g_triangleSpv, sizeof(g_triangleSpv), "fragmentMain")
                              .SetTopology(VriPrimitiveTopology_TriangleList)
                              .SetCullMode(VriCullMode_None)
                              .AddColorAttachment(app.ColorFormat())
                              .Build(device);
    if (!pipelineResult)
    {
        std::fprintf(stderr, "[vrf-triangle] %s\n", pipelineResult.error().message.c_str());
        return 1;
    }
    VriPipeline* pipeline = *pipelineResult;

    app.onRecord = [&](VriCommandBuffer* cmd) {
        const VriCoreInterface& c = device.Core();
        c.CmdSetPipeline(cmd, pipeline);
        VriDrawDesc draw {};
        draw.vertexNum   = 3;
        draw.instanceNum = 1;
        c.CmdDraw(cmd, &draw);
    };

    std::printf("[vrf-triangle] %s\n", vrf::DescribeDevice(device).c_str());
    app.Run();

    const VriCoreInterface& c = device.Core();
    c.DestroyPipeline(pipeline);
    c.DestroyPipelineLayout(layout);
    return 0;
}
