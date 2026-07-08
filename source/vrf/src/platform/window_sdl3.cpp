#if defined(VRF_WINDOW_SDL3)

#include "window_backends.hpp"

#include <string>
#include <utility>

#include <SDL3/SDL.h>
#include <vri/integration/vri_sdl3.h>

#if defined(VRF_WITH_IMGUI)
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#endif

namespace vrf
{
    namespace
    {
        class WindowSDL3 final : public Window
        {
        public:
            WindowSDL3(SDL_Window* window, std::string title) : m_window(window), m_title(std::move(title))
            {
                int wpx = 0, hpx = 0;
                SDL_GetWindowSizeInPixels(m_window, &wpx, &hpx);
                m_extent = {static_cast<uint32_t>(wpx), static_cast<uint32_t>(hpx)};
            }

            ~WindowSDL3() override
            {
                if (m_window)
                    SDL_DestroyWindow(m_window);
                SDL_Quit();
            }

            VriWindowHandle Handle() const override { return vriWindowHandleFromSDL3(m_window); }

            const void* NativeDisplay() const override
            {
                const VriWindowHandle h = vriWindowHandleFromSDL3(m_window);
                return h.type == VriWindowSystem_Wayland ? h.handle.wayland.display : nullptr;
            }

            Extent2D      Extent() const override { return m_extent; }
            WindowBackend Backend() const override { return WindowBackend::SDL3; }
            bool          ShouldClose() const override { return m_shouldClose; }
            const char*   Title() const override { return m_title.c_str(); }

#if defined(VRF_WITH_IMGUI)
            bool InitImGui() override
            {
                m_imguiInitialized = ImGui_ImplSDL3_InitForOther(m_window);
                return m_imguiInitialized;
            }
            void NewImGuiFrame() override { ImGui_ImplSDL3_NewFrame(); }
            void ShutdownImGui() override
            {
                ImGui_ImplSDL3_Shutdown();
                m_imguiInitialized = false;
            }
#endif

            void PollEvents() override
            {
                SDL_Event e;
                while (SDL_PollEvent(&e))
                {
#if defined(VRF_WITH_IMGUI)
                    // Only feed ImGui once its context + SDL3 backend are initialized; a bare
                    // VRF_WITH_IMGUI app that never enables ImGui (e.g. the triangle) has no
                    // ImGui context and ProcessEvent would dereference null.
                    if (m_imguiInitialized)
                        ImGui_ImplSDL3_ProcessEvent(&e);
#endif
                    switch (e.type)
                    {
                        case SDL_EVENT_QUIT:
                        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                            m_shouldClose = true;
                            break;
                        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        case SDL_EVENT_WINDOW_RESIZED: {
                            int wpx = 0, hpx = 0;
                            SDL_GetWindowSizeInPixels(m_window, &wpx, &hpx);
                            m_extent = {static_cast<uint32_t>(wpx), static_cast<uint32_t>(hpx)};
                            break;
                        }
                        default:
                            break;
                    }
                }
            }

        private:
            SDL_Window* m_window = nullptr;
            std::string m_title;
            Extent2D    m_extent {};
            bool        m_shouldClose = false;
#if defined(VRF_WITH_IMGUI)
            bool m_imguiInitialized = false;
#endif
        };
    } // namespace

    namespace detail
    {
        Expected<std::unique_ptr<Window>> CreateWindowSDL3(const WindowDesc& desc)
        {
            if (!SDL_Init(SDL_INIT_VIDEO))
                return MakeError(std::string("SDL_Init failed: ") + SDL_GetError());

            SDL_WindowFlags flags = 0;
            if (desc.resizable)
                flags |= SDL_WINDOW_RESIZABLE;

            SDL_Window* window = SDL_CreateWindow(
                desc.title.c_str(), static_cast<int>(desc.extent.width), static_cast<int>(desc.extent.height), flags);
            if (!window)
            {
                const std::string err = SDL_GetError();
                SDL_Quit();
                return MakeError("SDL_CreateWindow failed: " + err);
            }
            SDL_RaiseWindow(window);

            return std::unique_ptr<Window>(std::make_unique<WindowSDL3>(window, desc.title));
        }
    } // namespace detail
} // namespace vrf

#endif // VRF_WINDOW_SDL3
