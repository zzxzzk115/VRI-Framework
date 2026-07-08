/*
 * render_module.hpp - the embeddable rendering layer.
 *
 * RenderModule bundles a device + swapchain + per-frame command machinery + an
 * optional depth target, driven by the HOST's loop. It deliberately does NOT own
 * a window or an event loop: the host (an engine, editor, or vrf::Application)
 * supplies a native VriWindowHandle and calls BeginFrame -> record -> EndFrame.
 * This is how vrf embeds into a larger renderer/engine as a module / render server.
 *
 * RenderModule is non-movable and heap-owned (Create returns a unique_ptr): the
 * swapchain and frame hold pointers back to the embedded RenderDevice.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"
#include "vrf/gpu/frame.hpp"
#include "vrf/gpu/render_device.hpp"
#include "vrf/gpu/swapchain.hpp"

#if defined(VRF_WITH_IMGUI)
struct ImDrawData; // Dear ImGui (global namespace); the full type is only needed in the .cpp
#endif

namespace vrf
{
    struct RenderModuleDesc
    {
        GraphicsApi     api             = GraphicsApi::Auto;
        bool            validation      = true;
        uint64_t        enabledFeatures = 0;
        VriWindowHandle window {};               // native handle from the host's windowing
        const void*     nativeDisplay = nullptr; // host's wl_display on Wayland; nullptr elsewhere
        Extent2D        extent {};
        VriFormat       colorFormat   = VriFormat_BGRA8_UNORM;
        VriFormat       depthFormat   = VriFormat_Unknown; // Unknown = no depth
        VriPresentMode  presentMode   = VriPresentMode_Fifo;
        float           clearColor[4] = {0.08f, 0.10f, 0.14f, 1.0f};
        float           clearDepth    = 1.0f;
        bool            enableImGui   = false; // query VRI's imgui renderer (needs VRF_WITH_IMGUI)
    };

    // Handed back by BeginFrame: a command buffer already inside the default render pass
    // (color [+ depth] bound and cleared, viewport/scissor set). Record draws into `cmd`.
    struct FrameContext
    {
        bool              valid      = false; // false => acquire was out of date; Resize() and skip
        VriCommandBuffer* cmd        = nullptr;
        VriTexture*       backbuffer = nullptr;
        VriDescriptor*    colorView  = nullptr;
        Extent2D          extent {};
    };

    class RenderModule
    {
    public:
        ~RenderModule();

        RenderModule(const RenderModule&)            = delete;
        RenderModule& operator=(const RenderModule&) = delete;
        RenderModule(RenderModule&&)                 = delete;
        RenderModule& operator=(RenderModule&&)      = delete;

        [[nodiscard]] static Expected<std::unique_ptr<RenderModule>> Create(const RenderModuleDesc& desc);

        // Acquire the next backbuffer and open the default pass. If the result is not valid,
        // the swapchain is out of date: call Resize() with the current window size and skip.
        [[nodiscard]] FrameContext BeginFrame();

        // Close the pass, submit, and present. Pair with a valid BeginFrame().
        void EndFrame();

        void Resize(Extent2D extent);

        [[nodiscard]] RenderDevice& Device() noexcept { return m_device; }
        [[nodiscard]] Swapchain&    GetSwapchain() noexcept { return m_swapchain; }
        [[nodiscard]] VriFormat     ColorFormat() const noexcept { return m_desc.colorFormat; }
        [[nodiscard]] VriFormat     DepthFormat() const noexcept { return m_desc.depthFormat; }
        [[nodiscard]] bool          HasDepth() const noexcept { return m_desc.depthFormat != VriFormat_Unknown; }

        void SetClearColor(float r, float g, float b, float a = 1.0f);

#if defined(VRF_WITH_IMGUI)
        // Whether VRI's imgui renderer is created and ready to draw.
        [[nodiscard]] bool ImGuiEnabled() const noexcept { return m_imguiReady; }
        // Create the imgui renderer with a font atlas (RGBA8, from io.Fonts->GetTexDataAsRGBA32);
        // returns the font texture view to set as ImGui's default texture id.
        [[nodiscard]] Expected<VriDescriptor*> InitImGui(const void* fontPixels, uint32_t width, uint32_t height);
        // Flatten + stage this frame's ImGui geometry. Call after ImGui::Render(), before BeginFrame().
        void UploadImGui(ImDrawData* drawData);
#endif

    private:
        RenderModule() = default;

        void RecreateDepth(Extent2D extent);
        void DestroyDepth();

        RenderModuleDesc m_desc;
        RenderDevice     m_device;
        Swapchain        m_swapchain;
        Frame            m_frame;
        VriTexture*      m_depthTexture = nullptr;
        VriDescriptor*   m_depthView    = nullptr;

        // transient per-frame state (valid between BeginFrame and EndFrame)
        VriTexture*    m_backbuffer = nullptr;
        VriDescriptor* m_colorView  = nullptr;
        bool           m_inFrame    = false;

#if defined(VRF_WITH_IMGUI)
        VriImguiInterface                m_guiApi {};
        VriImgui*                        m_gui            = nullptr;
        bool                             m_imguiRequested = false;
        bool                             m_imguiReady     = false;
        bool                             m_guiHasData     = false;
        std::vector<VriImguiVertex>      m_guiVertices;
        std::vector<uint16_t>            m_guiIndices;
        std::vector<VriImguiDrawCommand> m_guiCommands;
        VriImguiDrawData                 m_guiData {};
#endif
    };
} // namespace vrf
