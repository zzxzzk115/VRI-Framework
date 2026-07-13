// vrf xr_triangle - validation example for the vrf::xr module and the
// probe-first / lazy-session architecture:
//
//   1. XrSystem::Probe() at startup. Success => the RenderDevice is created
//      XR-compatible through the runtime's Vulkan creation hooks; failure (or
//      --sim) => a plain device + SimStereoRig.
//   2. The SAME multiview render path draws into whichever rig is active: one
//      pass, viewMask 0b11, SV_ViewID tints left/right.
//   3. XR mode: XrSession paces on xrWaitFrame and submits the projection
//      layer. Sim mode: the eye array is composited side-by-side to a window.
//
// Run with an OpenXR runtime (headset / Monado / Meta XR Simulator), or with
// --sim on any desktop.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <vrf/core/log.hpp>
#include <vrf/fg/render_context.hpp>
#include <vrf/gpu/builders/graphics_pipeline_builder.hpp>
#include <vrf/gpu/builders/pipeline_layout_builder.hpp>
#include <vrf/gpu/frame_stream.hpp>
#include <vrf/gpu/render_device.hpp>
#include <vrf/gpu/shader_library.hpp>
#include <vrf/gpu/swapchain.hpp>
#include <vrf/platform/window.hpp>
#include <vrf/xr/sim_stereo_rig.hpp>
#include <vrf/xr/stereo_rig.hpp>
#if defined(VRF_WITH_OPENXR)
#include <vrf/xr/xr_session.hpp>
#include <vrf/xr/xr_system.hpp>
#endif

namespace
{
    const unsigned char g_xrTriangleVshlib[] = {
#include "xr_triangle.vshlib.h"
    };

    [[nodiscard]] vrf::Expected<vrf::fg::PassPipeline>
    makePipeline(vrf::RenderDevice&                                   device,
                 vrf::ShaderLibrary&                                  shaders,
                 const char*                                          shaderId,
                 const std::vector<vrf::fg::PipelineLayoutInfo::Set>& sets,
                 VriFormat                                            colorFormat,
                 uint32_t                                             viewMask)
    {
        auto vs = shaders.Resolve(shaderId, vrf::ShaderStage::Vertex, {});
        if (!vs)
            return std::unexpected(vs.error());
        auto fs = shaders.Resolve(shaderId, vrf::ShaderStage::Fragment, {});
        if (!fs)
            return std::unexpected(fs.error());

        vrf::PipelineLayoutBuilder layoutBuilder;
        layoutBuilder.SetShaderStages(VriShaderStage_AllGraphics);
        for (const auto& set : sets)
            layoutBuilder.AddDescriptorSet(set.registerSpace, set.ranges);
        auto layout = layoutBuilder.Build(device);
        if (!layout)
            return std::unexpected(layout.error());

        auto pipeline = vrf::GraphicsPipelineBuilder {}
                            .SetPipelineLayout(*layout)
                            .AddShader(VriShaderStage_Vertex, vs->spirv, vs->spirvSize, vs->entryPoint.c_str())
                            .AddShader(VriShaderStage_Fragment, fs->spirv, fs->spirvSize, fs->entryPoint.c_str())
                            .SetTopology(VriPrimitiveTopology_TriangleList)
                            .SetCullMode(VriCullMode_None)
                            .AddColorAttachment(colorFormat)
                            .SetViewMask(viewMask)
                            .Build(device);
        if (!pipeline)
            return std::unexpected(pipeline.error());

        auto layoutInfo    = std::make_shared<vrf::fg::PipelineLayoutInfo>();
        layoutInfo->handle = *layout;
        layoutInfo->sets   = sets;
        return vrf::fg::PassPipeline {*pipeline, std::move(layoutInfo), false};
    }

    // The single stereo pass both rigs share: clear + multiview triangle.
    void recordStereoPass(vrf::fg::RenderContext& rc, vrf::xr::StereoFrame& frame, const vrf::fg::PassPipeline& pipeline)
    {
        rc.TransitionTexture(*frame.colorTarget,
                             {VriAccess_ColorAttachmentWrite,
                              VriLayout_ColorAttachment,
                              VriPipelineStage_ColorAttachmentOutput});

        vrf::fg::FramebufferInfo fb;
        fb.area     = frame.extent;
        fb.layers   = 1;
        fb.viewMask = 0x3; // both eyes in one pass
        fb.colorAttachments.push_back(vrf::fg::AttachmentInfo {
            .target     = frame.colorTarget,
            .clearValue = glm::vec4 {0.05f, 0.05f, 0.08f, 1.0f},
        });
        rc.framebufferInfo = fb;

        rc.BeginRendering();
        rc.BindPipeline(pipeline);
        const VriDrawDesc draw {.vertexNum = 3, .instanceNum = 1};
        rc.device.Core().CmdDraw(rc.cmd, &draw);
        rc.EndRendering();
    }
} // namespace

int main(int argc, char** argv)
{
    bool forceSim = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--sim") == 0)
            forceSim = true;
    }

    // ---- 1. probe FIRST, then create the device accordingly ---------------
#if defined(VRF_WITH_OPENXR)
    std::optional<vrf::xr::XrSystem> xrSystem;
    if (!forceSim)
    {
        auto probed = vrf::xr::XrSystem::Probe({.applicationName = "vrf-xr-triangle"});
        if (probed)
        {
            xrSystem.emplace(std::move(*probed));
        }
        else
        {
            std::printf("[xr-triangle] %s -> simulation mode\n", probed.error().message.c_str());
        }
    }
#else
    struct
    {
        [[nodiscard]] constexpr explicit operator bool() const { return false; }
    } xrSystem;
    if (!forceSim)
        std::printf("[xr-triangle] built without vrf_with_openxr -> simulation mode\n");
#endif

    vrf::RenderDeviceDesc deviceDesc;
    deviceDesc.api        = vrf::GraphicsApi::Vulkan;
    deviceDesc.validation = true;
#if defined(VRF_WITH_OPENXR)
    if (xrSystem)
    {
        deviceDesc.nativeCreateInfo = xrSystem->VulkanCreateHooks();
    }
#endif
    auto deviceResult = vrf::RenderDevice::Create(deviceDesc);
    if (!deviceResult)
    {
        std::fprintf(stderr, "[xr-triangle] device: %s\n", deviceResult.error().message.c_str());
        return 1;
    }
    vrf::RenderDevice& device = *deviceResult;
    if (device.Desc()->hasMultiview == VRI_FALSE)
    {
        std::fprintf(stderr, "[xr-triangle] device has no multiview - cannot do single-pass stereo\n");
        return 1;
    }

    auto shadersResult = vrf::ShaderLibrary::LoadFromMemory(g_xrTriangleVshlib, sizeof(g_xrTriangleVshlib));
    if (!shadersResult)
    {
        std::fprintf(stderr, "[xr-triangle] shaders: %s\n", shadersResult.error().message.c_str());
        return 1;
    }
    vrf::ShaderLibrary shaders = std::move(*shadersResult);

    constexpr uint32_t kFramesInFlight = 2;
    auto               framesResult    = vrf::FrameStream::Create(device, kFramesInFlight);
    if (!framesResult)
    {
        std::fprintf(stderr, "[xr-triangle] frames: %s\n", framesResult.error().message.c_str());
        return 1;
    }
    vrf::FrameStream frames = std::move(*framesResult);

    std::vector<vrf::fg::DescriptorAllocator> descriptorAllocators;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        descriptorAllocators.emplace_back(device);
    vrf::fg::Samplers samplers;

    const bool autoExit      = std::getenv("VRF_EXAMPLE_AUTO_EXIT") != nullptr;
    const auto start         = std::chrono::steady_clock::now();
    const auto autoExitAfter = std::chrono::seconds(5);
    uint64_t   frameCount    = 0;

#if defined(VRF_WITH_OPENXR)
    if (xrSystem)
    {
        // ---- XR mode: lazy session (here: immediately; an app would gate this
        // on an "Enter VR" button), no desktop window. --------------------------
        auto sessionResult = vrf::xr::XrSession::Create(*xrSystem, device);
        if (!sessionResult)
        {
            std::fprintf(stderr, "[xr-triangle] session: %s\n", sessionResult.error().message.c_str());
            return 1;
        }
        auto& session = **sessionResult;

        auto stereoPipeline = makePipeline(device, shaders, "xr_stereo", {}, session.ColorFormat(), 0x3);
        if (!stereoPipeline)
        {
            std::fprintf(stderr, "[xr-triangle] pipeline: %s\n", stereoPipeline.error().message.c_str());
            return 1;
        }

        std::printf("[xr-triangle] XR mode: %ux%u per eye. Left eye red, right eye green.\n",
                    session.EyeExtent().width,
                    session.EyeExtent().height);

        while (session.IsRunning())
        {
            auto frameResult = session.BeginFrame();
            if (!frameResult)
                break;
            auto& frame = *frameResult;

            if (frame.shouldRender)
            {
                VriCommandBuffer* cmd           = frames.Begin();
                auto&             slotAllocator = descriptorAllocators[frames.FrameIndex()];
                slotAllocator.Reset();
                vrf::fg::RenderContext rc {device, cmd, samplers, slotAllocator};

                recordStereoPass(rc, frame, *stereoPipeline);
                session.PreSubmit(cmd, frame);
                frames.Submit();
                ++frameCount;
            }
            session.EndFrame(frame);

            if (autoExit && std::chrono::steady_clock::now() - start > autoExitAfter)
                break;
        }
        frames.WaitAll();
        device.WaitIdle();
        device.Core().DestroyPipeline(stereoPipeline->pipeline);
        device.Core().DestroyPipelineLayout(stereoPipeline->layout->handle);
        std::printf("[xr-triangle] XR frames rendered: %llu\n", static_cast<unsigned long long>(frameCount));
        return 0;
    }
#endif

    // ---- simulation mode: SimStereoRig + side-by-side desktop composite -----
    constexpr vrf::Extent2D kWindowExtent {1280, 640};

    vrf::WindowDesc windowDesc;
    windowDesc.title  = "vrf xr_triangle (simulation)";
    windowDesc.extent = kWindowExtent;
    auto windowResult = vrf::Window::Create(windowDesc);
    if (!windowResult)
    {
        std::fprintf(stderr, "[xr-triangle] window: %s\n", windowResult.error().message.c_str());
        return 1;
    }
    auto& window = *windowResult.value();

    vrf::SwapchainDesc swapDesc;
    swapDesc.window = window.Handle();
    swapDesc.extent = kWindowExtent;
    auto swapResult = vrf::Swapchain::Create(device, swapDesc);
    if (!swapResult)
    {
        std::fprintf(stderr, "[xr-triangle] swapchain: %s\n", swapResult.error().message.c_str());
        return 1;
    }
    vrf::Swapchain& swapchain = *swapResult;

    auto rigResult = vrf::xr::SimStereoRig::Create(device, {.eyeExtent = {640, 640}});
    if (!rigResult)
    {
        std::fprintf(stderr, "[xr-triangle] sim rig: %s\n", rigResult.error().message.c_str());
        return 1;
    }
    vrf::xr::SimStereoRig& rig = **rigResult;

    {
        VriSamplerDesc samplerDesc {};
        samplerDesc.magFilter    = VriFilter_Linear;
        samplerDesc.minFilter    = VriFilter_Linear;
        samplerDesc.addressModeU = VriAddressMode_ClampToEdge;
        samplerDesc.addressModeV = VriAddressMode_ClampToEdge;
        samplerDesc.addressModeW = VriAddressMode_ClampToEdge;
        VriDescriptor* sampler   = nullptr;
        if (!vrf::Succeeded(device.Core().CreateSampler(device.Handle(), &samplerDesc, &sampler)))
        {
            std::fprintf(stderr, "[xr-triangle] sampler creation failed\n");
            return 1;
        }
        samplers["linearClamp"] = sampler;
    }

    auto stereoPipeline = makePipeline(device, shaders, "xr_stereo", {}, rig.ColorFormat(), 0x3);

    using Set          = vrf::fg::PipelineLayoutInfo::Set;
    auto compositeSets = std::vector<Set> {
        Set {0,
             {
                 {.baseRegister   = 0,
                  .descriptorNum  = 1,
                  .descriptorType = VriDescriptorType_Texture,
                  .shaderStages   = VriShaderStage_Fragment,
                  .viewType       = VriTextureViewType_2DArray},
                 {.baseRegister   = 1,
                  .descriptorNum  = 1,
                  .descriptorType = VriDescriptorType_Sampler,
                  .shaderStages   = VriShaderStage_Fragment},
             }},
    };
    auto compositePipeline = makePipeline(device, shaders, "xr_composite", compositeSets, swapchain.Format(), 0);
    if (!stereoPipeline || !compositePipeline)
    {
        const auto& err = !stereoPipeline ? stereoPipeline.error() : compositePipeline.error();
        std::fprintf(stderr, "[xr-triangle] pipeline: %s\n", err.message.c_str());
        return 1;
    }

    std::vector<vrf::fg::Texture> backbuffers;
    auto rebuildBackbuffers = [&] {
        backbuffers.clear();
        for (uint32_t i = 0; i < swapchain.TextureCount(); ++i)
        {
            backbuffers.push_back(vrf::fg::Texture::Borrow(
                device,
                swapchain.Texture(i),
                {.extent = swapchain.Extent(), .format = swapchain.Format(), .usage = VriTextureUsage_ColorAttachment},
                {VriAccess_None, VriLayout_Undefined, VriPipelineStage_None}));
        }
    };
    rebuildBackbuffers();

    std::printf("[xr-triangle] simulation mode: %ux%u per eye, side-by-side in the window.\n",
                rig.EyeExtent().width,
                rig.EyeExtent().height);

    while (!window.ShouldClose())
    {
        window.PollEvents();

        if (window.Extent() != swapchain.Extent() && window.Extent().width > 0 && window.Extent().height > 0)
        {
            frames.WaitAll();
            device.WaitIdle();
            swapchain.Resize(window.Extent());
            rebuildBackbuffers();
        }

        const auto acquired = swapchain.Acquire();
        if (acquired.outOfDate)
        {
            frames.WaitAll();
            device.WaitIdle();
            swapchain.Resize(window.Extent());
            rebuildBackbuffers();
            continue;
        }

        auto frameResult = rig.BeginFrame();
        auto& frame      = *frameResult;

        VriCommandBuffer* cmd           = frames.Begin();
        auto&             slotAllocator = descriptorAllocators[frames.FrameIndex()];
        slotAllocator.Reset();
        vrf::fg::RenderContext rc {device, cmd, samplers, slotAllocator};

        recordStereoPass(rc, frame, *stereoPipeline);

        // Composite the 2-layer eye target side-by-side into the backbuffer.
        rc.TransitionTexture(*frame.colorTarget,
                             {VriAccess_ShaderResourceRead, VriLayout_ShaderResource, VriPipelineStage_FragmentShader});
        auto& backbuffer = backbuffers[acquired.index];
        rc.TransitionTexture(
            backbuffer,
            {VriAccess_ColorAttachmentWrite, VriLayout_ColorAttachment, VriPipelineStage_ColorAttachmentOutput});

        vrf::fg::FramebufferInfo fb;
        fb.area = swapchain.Extent();
        fb.colorAttachments.push_back(vrf::fg::AttachmentInfo {.target = &backbuffer});
        rc.framebufferInfo = fb;

        rc.resourceSet[0][0] = vrf::fg::bindings::SampledImage {frame.colorTarget};
        rc.SetSampler(0, 1, samplers["linearClamp"]);
        rc.RenderFullScreenPostProcess(*compositePipeline);

        rc.TransitionTexture(backbuffer, {VriAccess_None, VriLayout_Present, VriPipelineStage_AllCommands});

        rig.PreSubmit(cmd, frame);
        frames.Submit();
        swapchain.Present();
        rig.EndFrame(frame);
        ++frameCount;

        if (autoExit && std::chrono::steady_clock::now() - start > autoExitAfter)
            break;
    }

    frames.WaitAll();
    device.WaitIdle();

    for (auto& [_, sampler] : samplers)
        device.Core().DestroyDescriptor(sampler);
    for (const auto* pipeline : {&*stereoPipeline, &*compositePipeline})
    {
        device.Core().DestroyPipeline(pipeline->pipeline);
        device.Core().DestroyPipelineLayout(pipeline->layout->handle);
    }

    std::printf("[xr-triangle] sim frames rendered: %llu\n", static_cast<unsigned long long>(frameCount));
    return 0;
}
