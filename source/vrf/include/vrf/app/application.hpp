/*
 * application.hpp - a window + present loop convenience built on RenderModule.
 *
 * Application owns a vrf::Window and drives a RenderModule's frame loop until the
 * window closes. It is the "just run it" entry point; an engine that already owns
 * its window and loop should embed RenderModule directly instead.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"
#include "vrf/gpu/render_module.hpp"
#include "vrf/platform/window.hpp"

namespace vrf
{
    struct ApplicationDesc
    {
        std::string    title           = "vrf";
        Extent2D       extent          = {1280, 720};
        WindowBackend  windowBackend   = WindowBackend::Auto;
        GraphicsApi    api             = GraphicsApi::Auto;
        bool           validation      = true;
        uint64_t       enabledFeatures = 0; // opt-in VriFeatureBits to request (best-effort; see VriFeature_*)
        VriFormat      colorFormat     = VriFormat_BGRA8_UNORM;
        VriFormat      depthFormat     = VriFormat_Unknown;
        VriPresentMode presentMode     = VriPresentMode_Fifo;
        float          clearColor[4]   = {0.08f, 0.10f, 0.14f, 1.0f};
        float          clearDepth      = 1.0f;
        bool           imgui           = false; // enable the Dear ImGui integration (needs VRF_WITH_IMGUI)
    };

    class Application
    {
    public:
        ~Application();

        Application(const Application&)            = delete;
        Application& operator=(const Application&) = delete;
        Application(Application&&)                 = delete;
        Application& operator=(Application&&)      = delete;

        [[nodiscard]] static Expected<std::unique_ptr<Application>> Create(const ApplicationDesc& desc);

        // Record draw commands inside the render pass (pipeline + draws). Optional.
        std::function<void(VriCommandBuffer* cmd)> onRecord;

        // Build the ImGui UI for the frame (between NewFrame and Render). Only called when
        // ApplicationDesc::imgui is set and the framework is built with VRF_WITH_IMGUI.
        std::function<void()> onGui;

        void Run();

        [[nodiscard]] Window&       GetWindow() noexcept { return *m_window; }
        [[nodiscard]] RenderModule& Module() noexcept { return *m_module; }
        [[nodiscard]] RenderDevice& Device() noexcept { return m_module->Device(); }
        [[nodiscard]] Swapchain&    GetSwapchain() noexcept { return m_module->GetSwapchain(); }
        [[nodiscard]] VriFormat     ColorFormat() const noexcept { return m_module->ColorFormat(); }
        [[nodiscard]] VriFormat     DepthFormat() const noexcept { return m_module->DepthFormat(); }

        void SetClearColor(float r, float g, float b, float a = 1.0f);

    private:
        Application() = default;

#if defined(VRF_WITH_IMGUI)
        Expected<void> InitImGui();
#endif

        ApplicationDesc               m_desc;
        std::unique_ptr<Window>       m_window;
        std::unique_ptr<RenderModule> m_module;
#if defined(VRF_WITH_IMGUI)
        bool m_imguiActive = false;
#endif
    };
} // namespace vrf
