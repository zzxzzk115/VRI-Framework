/*
 * window.hpp - abstract OS window with SDL3 / GLFW backends.
 *
 * A Window owns a native OS window and produces the VriWindowHandle that the VRI
 * swapchain consumes. Which backend is instantiated is chosen by
 * WindowDesc::backend (Auto prefers SDL3 when compiled in); a backend is only
 * available if its xmake option (vrf_window_sdl3 / vrf_window_glfw) is enabled.
 */
#pragma once

#include <memory>
#include <string>

#include <vri/ext/vri_ext_swapchain.h> // VriWindowHandle

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    enum class WindowBackend
    {
        Auto, // prefer SDL3 if compiled in, otherwise GLFW
        SDL3,
        GLFW
    };

    struct WindowDesc
    {
        std::string   title     = "vrf";
        Extent2D      extent    = {1280, 720};
        WindowBackend backend   = WindowBackend::Auto;
        bool          resizable = true;
    };

    // The native handle of a window this library did NOT create. ImGui's platform backend spawns
    // its own OS windows for detached viewports, and a swapchain for one still needs a
    // VriWindowHandle - but what the backend leaves in ImGuiViewport::PlatformHandle differs by
    // backend: an SDL_WindowID for SDL3, a GLFWwindow* for GLFW. Resolving that is the one thing
    // only the active window backend can do.
    [[nodiscard]] Expected<VriWindowHandle> ForeignWindowHandle(WindowBackend backend, void* platformHandle);

    class Window
    {
    public:
        Window()          = default;
        virtual ~Window() = default;

        Window(const Window&)            = delete;
        Window& operator=(const Window&) = delete;

        [[nodiscard]] static Expected<std::unique_ptr<Window>> Create(const WindowDesc& desc);

        // Native window handle for VriSwapChainDesc::window.
        [[nodiscard]] virtual VriWindowHandle Handle() const = 0;

        // Native display connection shared with the device (wl_display* on Wayland;
        // nullptr elsewhere). Passed to VriDeviceCreationDesc::nativeDisplay.
        [[nodiscard]] virtual const void* NativeDisplay() const = 0;

        // Current framebuffer extent in pixels (tracks resizes after PollEvents).
        [[nodiscard]] virtual Extent2D Extent() const = 0;

        [[nodiscard]] virtual WindowBackend Backend() const = 0;

        // Pump the OS event queue; refreshes ShouldClose() and Extent().
        virtual void PollEvents() = 0;

        [[nodiscard]] virtual bool ShouldClose() const = 0;

        [[nodiscard]] virtual const char* Title() const = 0;

        // Set the OS window title bar text (e.g. vrf::Application appends the resolved backend).
        virtual void SetTitle(const char* title) = 0;

        // Optional Dear ImGui platform integration (input + per-frame). No-ops unless the
        // window backend wires them (built with VRF_WITH_IMGUI). Init returns false if
        // unavailable. Driven by vrf::Application when its ApplicationDesc::imgui is set.
        virtual bool InitImGui() { return false; }
        virtual void NewImGuiFrame() {}
        virtual void ShutdownImGui() {}
    };
} // namespace vrf
