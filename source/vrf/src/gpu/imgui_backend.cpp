#if defined(VRF_WITH_IMGUI)

#include "vrf/gpu/imgui_backend.hpp"

#include <utility>
#include <vector>

#include <imgui.h>

#include "vrf/core/log.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    ImGuiBackend::~ImGuiBackend()
    {
        DisableViewports();
        Reset();
    }

    ImGuiBackend::ImGuiBackend(ImGuiBackend&& other) noexcept :
        m_device {std::exchange(other.m_device, nullptr)}, m_api {other.m_api},
        m_gui {std::exchange(other.m_gui, nullptr)}, m_fontView {std::exchange(other.m_fontView, nullptr)},
        m_hasData {other.m_hasData}, m_vertices {std::move(other.m_vertices)}, m_indices {std::move(other.m_indices)},
        m_commands {std::move(other.m_commands)}, m_data {other.m_data}
    {}

    ImGuiBackend& ImGuiBackend::operator=(ImGuiBackend&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device   = std::exchange(other.m_device, nullptr);
            m_api      = other.m_api;
            m_gui      = std::exchange(other.m_gui, nullptr);
            m_fontView = std::exchange(other.m_fontView, nullptr);
            m_hasData  = other.m_hasData;
            m_vertices = std::move(other.m_vertices);
            m_indices  = std::move(other.m_indices);
            m_commands = std::move(other.m_commands);
            m_data     = other.m_data;
        }
        return *this;
    }

    void ImGuiBackend::Reset() noexcept
    {
        if (m_device && m_gui)
        {
            m_api.DestroyImgui(m_gui);
        }
        m_gui      = nullptr;
        m_fontView = nullptr;
        m_device   = nullptr;
    }

    Expected<ImGuiBackend> ImGuiBackend::Create(RenderDevice&   device,
                                                const VriFormat colorFormat,
                                                const VriFormat depthFormat,
                                                const void*     fontPixels,
                                                const uint32_t  fontWidth,
                                                const uint32_t  fontHeight)
    {
        ImGuiBackend out;
        out.m_device = &device;

        if (const auto r = vriGetInterface(device.Handle(), VRI_INTERFACE_IMGUI, sizeof(out.m_api), &out.m_api);
            !Succeeded(r))
        {
            return MakeError(r, "ImGuiBackend::Create", "imgui interface unavailable");
        }

        VriImguiDesc desc {};
        desc.uploadQueue = device.GraphicsQueue();
        desc.colorFormat = colorFormat;
        desc.depthFormat = depthFormat;
        desc.fontAtlas   = fontPixels;
        desc.fontWidth   = fontWidth;
        desc.fontHeight  = fontHeight;
        if (const auto r = out.m_api.CreateImgui(device.Handle(), &desc, &out.m_gui); !Succeeded(r))
        {
            return MakeError(r, "ImGuiBackend::Create", "CreateImgui failed");
        }
        out.m_fontView = out.m_api.GetImguiFontView(out.m_gui);
        return out;
    }

    namespace
    {
        // ImDrawData is per-list buffers with list-relative offsets; VriImguiDrawData is one buffer
        // each with global offsets. Shared by the main window and every detached viewport, which is
        // why it takes its output containers rather than writing members.
        void FlattenDrawData(ImDrawData*                       drawData,
                             std::vector<VriImguiVertex>&      vertices,
                             std::vector<uint16_t>&            indices,
                             std::vector<VriImguiDrawCommand>& commands,
                             VriImguiDrawData&                 data,
                             const uint32_t                    framebufferWidth,
                             const uint32_t                    framebufferHeight)
        {
            // Flatten ImDrawData (per-list buffers + list-relative offsets) into the
            // neutral VriImguiDrawData (one buffer each, global offsets).
            vertices.clear();
            indices.clear();
            commands.clear();
            data = {};
            if (!drawData || drawData->TotalVtxCount == 0)
            {
                return;
            }

            vertices.reserve(static_cast<size_t>(drawData->TotalVtxCount));
            indices.reserve(static_cast<size_t>(drawData->TotalIdxCount));
            uint32_t vertexBase = 0, indexBase = 0;
            for (int n = 0; n < drawData->CmdListsCount; ++n)
            {
                const ImDrawList* list = drawData->CmdLists[n];
                for (int v = 0; v < list->VtxBuffer.Size; ++v)
                {
                    const ImDrawVert& s = list->VtxBuffer.Data[v];
                    vertices.push_back(VriImguiVertex {{s.pos.x, s.pos.y}, {s.uv.x, s.uv.y}, s.col});
                }
                for (int i = 0; i < list->IdxBuffer.Size; ++i)
                {
                    indices.push_back(static_cast<uint16_t>(list->IdxBuffer.Data[i]));
                }
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
                    commands.push_back(o);
                }
                vertexBase += static_cast<uint32_t>(list->VtxBuffer.Size);
                indexBase += static_cast<uint32_t>(list->IdxBuffer.Size);
            }

            data.vertices          = vertices.data();
            data.vertexCount       = static_cast<uint32_t>(vertices.size());
            data.indices           = indices.data();
            data.indexCount        = static_cast<uint32_t>(indices.size());
            data.indexSize         = sizeof(uint16_t);
            data.commands          = commands.data();
            data.commandCount      = static_cast<uint32_t>(commands.size());
            data.displayPos[0]     = drawData->DisplayPos.x;
            data.displayPos[1]     = drawData->DisplayPos.y;
            data.displaySize[0]    = drawData->DisplaySize.x;
            data.displaySize[1]    = drawData->DisplaySize.y;
            data.framebufferWidth  = framebufferWidth;
            data.framebufferHeight = framebufferHeight;
        }
    } // namespace

    void ImGuiBackend::Upload(ImDrawData* drawData, const Extent2D framebufferExtent)
    {
        m_hasData = false;
        if (drawData == nullptr || drawData->TotalVtxCount == 0)
        {
            m_vertices.clear();
            m_indices.clear();
            m_commands.clear();
            m_data = {};
            return;
        }
        FlattenDrawData(
            drawData, m_vertices, m_indices, m_commands, m_data, framebufferExtent.width, framebufferExtent.height);
        m_api.UploadImguiData(m_gui, &m_data);
        m_hasData = true;
    }

    void ImGuiBackend::CmdCopy(VriCommandBuffer* cmd)
    {
        if (m_hasData)
        {
            m_api.CmdCopyImguiData(cmd, m_gui);
        }
    }

    void ImGuiBackend::CmdDraw(VriCommandBuffer* cmd)
    {
        if (m_hasData)
        {
            m_api.CmdDrawImgui(cmd, m_gui, &m_data);
        }
    }

    void ImGuiBackend::FreeTexture(VriDescriptor* textureView)
    {
        if (m_gui != nullptr && textureView != nullptr)
        {
            m_api.FreeImguiTexture(m_gui, textureView);
        }
    }
    // ---- multi-viewport ------------------------------------------------------------------
    //
    // ImGui's platform backend creates the detached OS windows; these hooks give each one a
    // swapchain and the command machinery to draw into it. RenderPlatformWindowsDefault() drives
    // them once per extra window per frame.
    //
    // Everything here is per viewport, including the staging: sharing the main window's would let
    // one window's geometry clobber another's between the copy and the draw.

    ImGuiBackend* ImGuiBackend::s_viewportOwner = nullptr;

    namespace
    {
        struct ViewportData
        {
            ImGuiBackend*                    owner     = nullptr;
            VriSwapChain*                    swapchain = nullptr;
            VriImguiViewport*                gui       = nullptr;
            VriCommandAllocator*             allocator = nullptr;
            VriCommandBuffer*                cmd       = nullptr;
            VriFence*                        fence     = nullptr;
            uint64_t                         frame     = 0;
            uint32_t                         width = 1, height = 1;
            std::vector<VriImguiVertex>      vertices;
            std::vector<uint16_t>            indices;
            std::vector<VriImguiDrawCommand> commands;
            VriImguiDrawData                 data {};
        };

        [[nodiscard]] ViewportData* DataOf(ImGuiViewport* viewport)
        {
            return viewport == nullptr ? nullptr : static_cast<ViewportData*>(viewport->RendererUserData);
        }
    } // namespace

    void ImGuiBackend::ViewportCreate(ImGuiViewport* viewport)
    {
        ImGuiBackend* owner = s_viewportOwner;
        if (owner == nullptr || owner->m_device == nullptr)
            return;

        auto* data   = new ViewportData();
        data->owner  = owner;
        data->width  = static_cast<uint32_t>(viewport->Size.x > 0.0f ? viewport->Size.x : 1.0f);
        data->height = static_cast<uint32_t>(viewport->Size.y > 0.0f ? viewport->Size.y : 1.0f);

        RenderDevice& device = *owner->m_device;
        const auto    handle = ForeignWindowHandle(owner->m_viewportBackend, viewport->PlatformHandle);
        if (!handle)
        {
            LogError("imgui viewport: {}", handle.error().message);
        }
        else
        {
            VriSwapChainDesc desc {};
            desc.window = *handle;
            // Presented on the same queue as the main window: a second present queue would need its
            // own synchronisation with no benefit for UI-only windows.
            desc.queue       = device.GraphicsQueue();
            desc.format      = owner->m_viewportFormat;
            desc.width       = data->width;
            desc.height      = data->height;
            desc.textureNum  = 3;
            desc.presentMode = VriPresentMode_Fifo;
            if (device.Swap().CreateSwapChain(device.Handle(), &desc, &data->swapchain) != VriResult_Success)
            {
                LogError("imgui viewport: swapchain creation failed");
                data->swapchain = nullptr;
            }
        }

        data->gui = owner->m_api.CreateImguiViewport(owner->m_gui);
        device.Core().CreateCommandAllocator(device.Handle(), VriQueueType_Graphics, &data->allocator);
        device.Core().CreateCommandBuffer(data->allocator, &data->cmd);
        device.Core().CreateFence(device.Handle(), 0, &data->fence);
        viewport->RendererUserData = data;
    }

    void ImGuiBackend::ViewportDestroy(ImGuiViewport* viewport)
    {
        ViewportData* data = DataOf(viewport);
        if (data == nullptr)
            return;
        RenderDevice& device = *data->owner->m_device;
        // The window can close while its last frame is still in flight, and every object below is
        // referenced by that submission.
        device.Core().DeviceWaitIdle(device.Handle());
        if (data->gui != nullptr)
            data->owner->m_api.DestroyImguiViewport(data->gui);
        if (data->fence != nullptr)
            device.Core().DestroyFence(data->fence);
        if (data->allocator != nullptr)
            device.Core().DestroyCommandAllocator(data->allocator);
        if (data->swapchain != nullptr)
            device.Swap().DestroySwapChain(data->swapchain);
        delete data;
        viewport->RendererUserData = nullptr;
    }

    void ImGuiBackend::ViewportSetSize(ImGuiViewport* viewport, ImVec2 size)
    {
        ViewportData* data = DataOf(viewport);
        if (data == nullptr || data->swapchain == nullptr)
            return;
        data->width  = static_cast<uint32_t>(size.x > 0.0f ? size.x : 1.0f);
        data->height = static_cast<uint32_t>(size.y > 0.0f ? size.y : 1.0f);
        data->owner->m_device->Swap().Resize(data->swapchain, data->width, data->height);
    }

    void ImGuiBackend::ViewportRender(ImGuiViewport* viewport, void*)
    {
        ViewportData* data = DataOf(viewport);
        if (data == nullptr || data->swapchain == nullptr)
            return;
        ImGuiBackend& owner  = *data->owner;
        RenderDevice& device = *owner.m_device;

        FlattenDrawData(
            viewport->DrawData, data->vertices, data->indices, data->commands, data->data, data->width, data->height);
        owner.m_api.UploadImguiDataTo(data->gui, &data->data);

        uint32_t index = 0;
        if (device.Swap().AcquireNextTexture(data->swapchain, nullptr, 0, &index) == VriResult_OutOfDate)
        {
            device.Swap().Resize(data->swapchain, data->width, data->height);
            return;
        }

        VriTexture* textures[8] = {};
        uint32_t    count       = 8;
        device.Swap().GetSwapChainTextures(data->swapchain, textures, &count);
        VriTexture* backbuffer = textures[index];

        VriTextureViewDesc viewDesc {};
        viewDesc.texture              = backbuffer;
        viewDesc.viewType             = VriTextureViewType_2D;
        viewDesc.format               = VriFormat_Unknown;
        viewDesc.aspect               = VriImageAspect_Color;
        VriDescriptor* backbufferView = nullptr;
        device.Core().CreateTextureView(device.Handle(), &viewDesc, &backbufferView);

        const auto barrier = [&](const VriAccessFlags        beforeAccess,
                                 const VriLayout             beforeLayout,
                                 const VriPipelineStageFlags beforeStages,
                                 const VriAccessFlags        afterAccess,
                                 const VriLayout             afterLayout,
                                 const VriPipelineStageFlags afterStages) {
            VriTextureBarrierDesc texture {};
            texture.texture       = backbuffer;
            texture.before.access = beforeAccess;
            texture.before.layout = beforeLayout;
            texture.before.stages = beforeStages;
            texture.after.access  = afterAccess;
            texture.after.layout  = afterLayout;
            texture.after.stages  = afterStages;
            texture.aspect        = VriImageAspect_Color;
            VriBarrierGroupDesc group {};
            group.textures   = &texture;
            group.textureNum = 1;
            device.Core().CmdBarrier(data->cmd, &group);
        };

        device.Core().ResetCommandAllocator(data->allocator);
        device.Core().BeginCommandBuffer(data->cmd);
        owner.m_api.CmdCopyImguiDataTo(data->cmd, data->gui);
        barrier(VriAccess_None,
                VriLayout_Undefined,
                VriPipelineStage_None,
                VriAccess_ColorAttachmentWrite,
                VriLayout_ColorAttachment,
                VriPipelineStage_ColorAttachmentOutput);

        VriAttachmentDesc color {};
        color.view    = backbufferView;
        color.loadOp  = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) ? VriAttachmentLoadOp_Load :
                                                                                 VriAttachmentLoadOp_Clear;
        color.storeOp = VriAttachmentStoreOp_Store;
        color.clearValue.color.f32[3] = 1.0f; // opaque black behind the UI
        VriAttachmentsDesc attachments {};
        attachments.colors            = &color;
        attachments.colorNum          = 1;
        attachments.renderArea.width  = data->width;
        attachments.renderArea.height = data->height;
        attachments.layerNum          = 1;
        device.Core().CmdBeginRendering(data->cmd, &attachments);
        const VriViewport area {0, 0, static_cast<float>(data->width), static_cast<float>(data->height), 0, 1};
        device.Core().CmdSetViewports(data->cmd, &area, 1);
        const VriRect scissor {0, 0, data->width, data->height};
        device.Core().CmdSetScissors(data->cmd, &scissor, 1);
        owner.m_api.CmdDrawImguiTo(data->cmd, owner.m_gui, data->gui, &data->data);
        device.Core().CmdEndRendering(data->cmd);

        barrier(VriAccess_ColorAttachmentWrite,
                VriLayout_ColorAttachment,
                VriPipelineStage_ColorAttachmentOutput,
                VriAccess_None,
                VriLayout_Present,
                VriPipelineStage_AllCommands);
        device.Core().EndCommandBuffer(data->cmd);

        VriFenceSubmitDesc signal {};
        signal.fence  = data->fence;
        signal.value  = ++data->frame;
        signal.stages = VriPipelineStage_AllCommands;
        VriQueueSubmitDesc submit {};
        submit.commandBuffers   = &data->cmd;
        submit.commandBufferNum = 1;
        submit.signalFences     = &signal;
        submit.signalFenceNum   = 1;
        device.Core().QueueSubmit(device.GraphicsQueue(), &submit);
        // Waited immediately: the allocator and the staging are single-buffered per viewport, so
        // the next frame cannot start recording until this one has been consumed. UI windows are
        // cheap enough that the stall does not show, and double-buffering them would mean tracking
        // frames-in-flight per detached window.
        device.Core().Wait(data->fence, data->frame);
        device.Core().DestroyDescriptor(backbufferView);
    }

    void ImGuiBackend::ViewportSwap(ImGuiViewport* viewport, void*)
    {
        ViewportData* data = DataOf(viewport);
        if (data != nullptr && data->swapchain != nullptr)
            data->owner->m_device->Swap().Present(data->swapchain, nullptr, 0);
    }

    Expected<void> ImGuiBackend::EnableViewports(const WindowBackend backend, const VriFormat swapchainFormat)
    {
        if (m_device == nullptr)
            return std::unexpected(Error {VriResult_Failure, "imgui viewports: backend not created"});
        if (s_viewportOwner != nullptr && s_viewportOwner != this)
            return std::unexpected(
                Error {VriResult_Failure, "imgui viewports: another ImGuiBackend already owns them"});
        if (backend == WindowBackend::Auto)
            return std::unexpected(
                Error {VriResult_InvalidArgument, "imgui viewports: pass the concrete window backend"});

        m_viewportBackend  = backend;
        m_viewportFormat   = swapchainFormat;
        m_viewportsEnabled = true;
        s_viewportOwner    = this;

        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
        ImGuiPlatformIO& platform       = ImGui::GetPlatformIO();
        platform.Renderer_CreateWindow  = ViewportCreate;
        platform.Renderer_DestroyWindow = ViewportDestroy;
        platform.Renderer_SetWindowSize = ViewportSetSize;
        platform.Renderer_RenderWindow  = ViewportRender;
        platform.Renderer_SwapBuffers   = ViewportSwap;
        return {};
    }

    void ImGuiBackend::DisableViewports() noexcept
    {
        if (!m_viewportsEnabled)
            return;
        // A host that destroys the ImGui context before this backend has already had its secondary
        // windows torn down - DestroyContext runs DestroyPlatformWindows itself, with our hooks
        // still registered - so there is nothing left to do and every ImGui call below would touch
        // freed state. This ran as a crash on exit before the guard.
        if (ImGui::GetCurrentContext() == nullptr)
        {
            m_viewportsEnabled = false;
            if (s_viewportOwner == this)
                s_viewportOwner = nullptr;
            return;
        }
        // Destroys every secondary window through ViewportDestroy, which waits the device idle.
        ImGui::DestroyPlatformWindows();

        ImGuiPlatformIO& platform       = ImGui::GetPlatformIO();
        platform.Renderer_CreateWindow  = nullptr;
        platform.Renderer_DestroyWindow = nullptr;
        platform.Renderer_SetWindowSize = nullptr;
        platform.Renderer_RenderWindow  = nullptr;
        platform.Renderer_SwapBuffers   = nullptr;
        ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;

        m_viewportsEnabled = false;
        if (s_viewportOwner == this)
            s_viewportOwner = nullptr;
    }

} // namespace vrf

#endif // VRF_WITH_IMGUI
