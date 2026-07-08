#include "vrf/gpu/render_module.hpp"

#include <utility>

#if defined(VRF_WITH_IMGUI)
#include <imgui.h>
#endif

namespace vrf
{
    Expected<std::unique_ptr<RenderModule>> RenderModule::Create(const RenderModuleDesc& desc)
    {
        RenderDeviceDesc deviceDesc;
        deviceDesc.api             = desc.api;
        deviceDesc.validation      = desc.validation;
        deviceDesc.enabledFeatures = desc.enabledFeatures;
        deviceDesc.nativeDisplay   = desc.nativeDisplay;

        auto device = RenderDevice::Create(deviceDesc);
        if (!device)
            return std::unexpected(device.error());

        auto module      = std::unique_ptr<RenderModule>(new RenderModule());
        module->m_desc   = desc;
        module->m_device = std::move(*device);

        SwapchainDesc swapchainDesc;
        swapchainDesc.window      = desc.window;
        swapchainDesc.extent      = desc.extent;
        swapchainDesc.format      = desc.colorFormat;
        swapchainDesc.presentMode = desc.presentMode;

        auto swapchain = Swapchain::Create(module->m_device, swapchainDesc);
        if (!swapchain)
            return std::unexpected(swapchain.error());
        module->m_swapchain = std::move(*swapchain);

        auto frame = Frame::Create(module->m_device);
        if (!frame)
            return std::unexpected(frame.error());
        module->m_frame = std::move(*frame);

#if defined(VRF_WITH_IMGUI)
        if (desc.enableImGui)
        {
            const VriResult r = vriGetInterface(
                module->m_device.Handle(), VRI_INTERFACE_IMGUI, sizeof(module->m_guiApi), &module->m_guiApi);
            module->m_imguiRequested = (r == VriResult_Success);
        }
#endif

        module->RecreateDepth(desc.extent);
        return module;
    }

    RenderModule::~RenderModule()
    {
        if (m_device)
        {
            m_device.WaitIdle();
#if defined(VRF_WITH_IMGUI)
            if (m_gui)
                m_guiApi.DestroyImgui(m_gui);
#endif
            DestroyDepth();
        }
    }

    void RenderModule::SetClearColor(float r, float g, float b, float a)
    {
        m_desc.clearColor[0] = r;
        m_desc.clearColor[1] = g;
        m_desc.clearColor[2] = b;
        m_desc.clearColor[3] = a;
    }

    void RenderModule::DestroyDepth()
    {
        const VriCoreInterface& c = m_device.Core();
        if (m_depthView)
            c.DestroyDescriptor(m_depthView);
        if (m_depthTexture)
            c.DestroyTexture(m_depthTexture);
        m_depthView    = nullptr;
        m_depthTexture = nullptr;
    }

    void RenderModule::RecreateDepth(Extent2D extent)
    {
        DestroyDepth();
        if (m_desc.depthFormat == VriFormat_Unknown || extent.IsZero())
            return;

        const VriCoreInterface& c = m_device.Core();

        VriTextureDesc td {};
        td.type                          = VriTextureType_2D;
        td.format                        = m_desc.depthFormat;
        td.width                         = extent.width;
        td.height                        = extent.height;
        td.depth                         = 1;
        td.mipNum                        = 1;
        td.layerNum                      = 1;
        td.sampleNum                     = 1;
        td.usage                         = VriTextureUsage_DepthStencilAttachment;
        td.memoryLocation                = VriMemoryLocation_Device;
        td.clearValue.depthStencil.depth = m_desc.clearDepth;
        c.CreateTexture(m_device.Handle(), &td, &m_depthTexture);

        VriTextureViewDesc vd {};
        vd.texture  = m_depthTexture;
        vd.viewType = VriTextureViewType_2D;
        vd.format   = VriFormat_Unknown;
        vd.aspect   = VriImageAspect_Depth;
        c.CreateTextureView(m_device.Handle(), &vd, &m_depthView);
    }

    void RenderModule::Resize(Extent2D extent)
    {
        m_swapchain.Resize(extent);
        RecreateDepth(m_swapchain.Extent());
    }

    FrameContext RenderModule::BeginFrame()
    {
        const VriCoreInterface& c = m_device.Core();

        const AcquireResult acquire = m_swapchain.Acquire();
        if (acquire.outOfDate)
            return {};

        m_backbuffer = m_swapchain.Texture(acquire.index);
        if (!m_backbuffer)
            return {};

        VriTextureViewDesc viewDesc {};
        viewDesc.texture  = m_backbuffer;
        viewDesc.viewType = VriTextureViewType_2D;
        viewDesc.format   = VriFormat_Unknown;
        viewDesc.aspect   = VriImageAspect_Color;
        m_colorView       = nullptr;
        c.CreateTextureView(m_device.Handle(), &viewDesc, &m_colorView);

        VriCommandBuffer* cmd = m_frame.Begin();

#if defined(VRF_WITH_IMGUI)
        // Staging -> device copy for the UI geometry, recorded OUTSIDE the render pass.
        if (m_imguiReady && m_guiHasData)
            m_guiApi.CmdCopyImguiData(cmd, m_gui);
#endif

        const bool hasDepth = HasDepth() && m_depthView != nullptr;

        VriTextureBarrierDesc barriers[2] {};
        barriers[0].texture       = m_backbuffer;
        barriers[0].before.layout = VriLayout_Undefined;
        barriers[0].before.stages = VriPipelineStage_None;
        barriers[0].after.access  = VriAccess_ColorAttachmentWrite;
        barriers[0].after.layout  = VriLayout_ColorAttachment;
        barriers[0].after.stages  = VriPipelineStage_ColorAttachmentOutput;
        barriers[0].aspect        = VriImageAspect_Color;
        uint32_t barrierNum       = 1;
        if (hasDepth)
        {
            barriers[1].texture       = m_depthTexture;
            barriers[1].before.layout = VriLayout_Undefined;
            barriers[1].before.stages = VriPipelineStage_None;
            barriers[1].after.access  = VriAccess_DepthStencilAttachmentWrite;
            barriers[1].after.layout  = VriLayout_DepthStencilAttachment;
            barriers[1].after.stages  = VriPipelineStage_EarlyFragmentTests;
            barriers[1].aspect        = VriImageAspect_Depth;
            barrierNum                = 2;
        }
        VriBarrierGroupDesc barrierGroup {};
        barrierGroup.textures   = barriers;
        barrierGroup.textureNum = barrierNum;
        c.CmdBarrier(cmd, &barrierGroup);

        const Extent2D extent = m_swapchain.Extent();

        VriAttachmentDesc color {};
        color.view                    = m_colorView;
        color.loadOp                  = VriAttachmentLoadOp_Clear;
        color.storeOp                 = VriAttachmentStoreOp_Store;
        color.clearValue.color.f32[0] = m_desc.clearColor[0];
        color.clearValue.color.f32[1] = m_desc.clearColor[1];
        color.clearValue.color.f32[2] = m_desc.clearColor[2];
        color.clearValue.color.f32[3] = m_desc.clearColor[3];

        VriAttachmentDesc depth {};
        depth.view                          = m_depthView;
        depth.loadOp                        = VriAttachmentLoadOp_Clear;
        depth.storeOp                       = VriAttachmentStoreOp_DontCare;
        depth.clearValue.depthStencil.depth = m_desc.clearDepth;

        VriAttachmentsDesc attachments {};
        attachments.colors   = &color;
        attachments.colorNum = 1;
        if (hasDepth)
            attachments.depth = &depth;
        attachments.renderArea.width  = extent.width;
        attachments.renderArea.height = extent.height;
        attachments.layerNum          = 1;
        c.CmdBeginRendering(cmd, &attachments);

        VriViewport viewport {
            0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
        c.CmdSetViewports(cmd, &viewport, 1);
        VriRect scissor {0, 0, extent.width, extent.height};
        c.CmdSetScissors(cmd, &scissor, 1);

        m_inFrame = true;

        FrameContext context;
        context.valid      = true;
        context.cmd        = cmd;
        context.backbuffer = m_backbuffer;
        context.colorView  = m_colorView;
        context.extent     = extent;
        return context;
    }

    void RenderModule::EndFrame()
    {
        if (!m_inFrame)
            return;

        const VriCoreInterface& c   = m_device.Core();
        VriCommandBuffer*       cmd = m_frame.Cmd();

#if defined(VRF_WITH_IMGUI)
        // UI draws go on top of the scene, INSIDE the render pass.
        if (m_imguiReady && m_guiHasData)
            m_guiApi.CmdDrawImgui(cmd, m_gui, &m_guiData);
#endif

        c.CmdEndRendering(cmd);

        VriTextureBarrierDesc toPresent {};
        toPresent.texture       = m_backbuffer;
        toPresent.before.access = VriAccess_ColorAttachmentWrite;
        toPresent.before.layout = VriLayout_ColorAttachment;
        toPresent.before.stages = VriPipelineStage_ColorAttachmentOutput;
        toPresent.after.layout  = VriLayout_Present;
        toPresent.after.stages  = VriPipelineStage_AllCommands;
        toPresent.aspect        = VriImageAspect_Color;
        VriBarrierGroupDesc toPresentGroup {};
        toPresentGroup.textures   = &toPresent;
        toPresentGroup.textureNum = 1;
        c.CmdBarrier(cmd, &toPresentGroup);

        m_frame.Submit();
        m_swapchain.Present();

        c.DestroyDescriptor(m_colorView);
        m_colorView  = nullptr;
        m_backbuffer = nullptr;
        m_inFrame    = false;
    }

#if defined(VRF_WITH_IMGUI)
    Expected<VriDescriptor*> RenderModule::InitImGui(const void* fontPixels, uint32_t width, uint32_t height)
    {
        if (!m_imguiRequested)
            return MakeError("RenderModule::InitImGui: imgui interface unavailable (enableImGui unset/unsupported)");

        VriImguiDesc gd {};
        gd.uploadQueue = m_device.GraphicsQueue();
        gd.colorFormat = m_desc.colorFormat;
        gd.depthFormat = HasDepth() ? m_desc.depthFormat : VriFormat_Unknown;
        gd.fontAtlas   = fontPixels;
        gd.fontWidth   = width;
        gd.fontHeight  = height;
        if (m_guiApi.CreateImgui(m_device.Handle(), &gd, &m_gui) != VriResult_Success)
            return MakeError("RenderModule::InitImGui: CreateImgui failed");
        m_imguiReady = true;
        return m_guiApi.GetImguiFontView(m_gui);
    }

    void RenderModule::UploadImGui(ImDrawData* drawData)
    {
        if (!m_imguiReady)
            return;

        // Flatten ImDrawData (per-list buffers + list-relative offsets) into the neutral
        // VriImguiDrawData the renderer consumes (one buffer each, global offsets).
        m_guiVertices.clear();
        m_guiIndices.clear();
        m_guiCommands.clear();
        m_guiData    = {};
        m_guiHasData = false;
        if (!drawData || drawData->TotalVtxCount == 0)
            return;

        m_guiVertices.reserve(static_cast<size_t>(drawData->TotalVtxCount));
        m_guiIndices.reserve(static_cast<size_t>(drawData->TotalIdxCount));
        uint32_t vertexBase = 0, indexBase = 0;
        for (int n = 0; n < drawData->CmdListsCount; ++n)
        {
            const ImDrawList* list = drawData->CmdLists[n];
            for (int v = 0; v < list->VtxBuffer.Size; ++v)
            {
                const ImDrawVert& s = list->VtxBuffer.Data[v];
                m_guiVertices.push_back(VriImguiVertex {{s.pos.x, s.pos.y}, {s.uv.x, s.uv.y}, s.col});
            }
            for (int i = 0; i < list->IdxBuffer.Size; ++i)
                m_guiIndices.push_back(static_cast<uint16_t>(list->IdxBuffer.Data[i]));
            for (int i = 0; i < list->CmdBuffer.Size; ++i)
            {
                const ImDrawCmd& dc = list->CmdBuffer[i];
                if (dc.UserCallback)
                {
                    dc.UserCallback(list, &dc);
                    continue;
                }
                VriImguiDrawCommand o {};
                o.clipRect[0]  = dc.ClipRect.x;
                o.clipRect[1]  = dc.ClipRect.y;
                o.clipRect[2]  = dc.ClipRect.z;
                o.clipRect[3]  = dc.ClipRect.w;
                o.indexCount   = dc.ElemCount;
                o.indexOffset  = indexBase + dc.IdxOffset;
                o.vertexOffset = static_cast<int32_t>(vertexBase + dc.VtxOffset);
                o.textureView  = reinterpret_cast<VriDescriptor*>(dc.GetTexID());
                m_guiCommands.push_back(o);
            }
            vertexBase += static_cast<uint32_t>(list->VtxBuffer.Size);
            indexBase += static_cast<uint32_t>(list->IdxBuffer.Size);
        }

        const Extent2D extent       = m_swapchain.Extent();
        m_guiData.vertices          = m_guiVertices.data();
        m_guiData.vertexCount       = static_cast<uint32_t>(m_guiVertices.size());
        m_guiData.indices           = m_guiIndices.data();
        m_guiData.indexCount        = static_cast<uint32_t>(m_guiIndices.size());
        m_guiData.indexSize         = sizeof(uint16_t);
        m_guiData.commands          = m_guiCommands.data();
        m_guiData.commandCount      = static_cast<uint32_t>(m_guiCommands.size());
        m_guiData.displayPos[0]     = drawData->DisplayPos.x;
        m_guiData.displayPos[1]     = drawData->DisplayPos.y;
        m_guiData.displaySize[0]    = drawData->DisplaySize.x;
        m_guiData.displaySize[1]    = drawData->DisplaySize.y;
        m_guiData.framebufferWidth  = extent.width;
        m_guiData.framebufferHeight = extent.height;

        m_guiApi.UploadImguiData(m_gui, &m_guiData);
        m_guiHasData = true;
    }
#endif // VRF_WITH_IMGUI
} // namespace vrf
