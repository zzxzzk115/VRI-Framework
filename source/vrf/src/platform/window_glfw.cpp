#if defined(VRF_WINDOW_GLFW)

#include "window_backends.hpp"

#include <string>
#include <utility>

#include <GLFW/glfw3.h>
#include <vri/integration/vri_glfw.h> // defines the native macros + includes glfw3native.h

#if defined(VRF_WITH_IMGUI)
#include <imgui.h>
#include <imgui_impl_glfw.h>
#endif

namespace vrf
{
    namespace
    {
        class WindowGLFW final : public Window
        {
        public:
            WindowGLFW(GLFWwindow* window, std::string title) : m_window(window), m_title(std::move(title))
            {
                glfwSetWindowUserPointer(m_window, this);
                glfwSetFramebufferSizeCallback(m_window, &WindowGLFW::OnFramebufferSize);
                int fw = 0, fh = 0;
                glfwGetFramebufferSize(m_window, &fw, &fh);
                m_extent = {static_cast<uint32_t>(fw), static_cast<uint32_t>(fh)};
            }

            ~WindowGLFW() override
            {
                if (m_window)
                    glfwDestroyWindow(m_window);
                glfwTerminate();
            }

            VriWindowHandle Handle() const override { return vriWindowHandleFromGLFW(m_window); }

            const void* NativeDisplay() const override
            {
                const VriWindowHandle h = vriWindowHandleFromGLFW(m_window);
                return h.type == VriWindowSystem_Wayland ? h.handle.wayland.display : nullptr;
            }

            Extent2D      Extent() const override { return m_extent; }
            WindowBackend Backend() const override { return WindowBackend::GLFW; }
            bool          ShouldClose() const override { return glfwWindowShouldClose(m_window) != 0; }
            const char*   Title() const override { return m_title.c_str(); }
            void          SetTitle(const char* title) override
            {
                m_title = title ? title : "";
                glfwSetWindowTitle(m_window, m_title.c_str());
            }

            void PollEvents() override { glfwPollEvents(); }

#if defined(VRF_WITH_IMGUI)
            // install_callbacks=true chains ImGui's input callbacks onto GLFW (and onto ours).
            bool InitImGui() override { return ImGui_ImplGlfw_InitForOther(m_window, true); }
            void NewImGuiFrame() override { ImGui_ImplGlfw_NewFrame(); }
            void ShutdownImGui() override { ImGui_ImplGlfw_Shutdown(); }
#endif

        private:
            static void OnFramebufferSize(GLFWwindow* window, int width, int height)
            {
                auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(window));
                if (self)
                    self->m_extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            }

            GLFWwindow* m_window = nullptr;
            std::string m_title;
            Extent2D    m_extent {};
        };
    } // namespace

    namespace detail
    {
        Expected<std::unique_ptr<Window>> CreateWindowGLFW(const WindowDesc& desc)
        {
            if (!glfwInit())
                return MakeError("glfwInit failed");

            // VRI owns the graphics API; GLFW must not create an OpenGL context for the window.
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

            GLFWwindow* window = glfwCreateWindow(static_cast<int>(desc.extent.width),
                                                  static_cast<int>(desc.extent.height),
                                                  desc.title.c_str(),
                                                  nullptr,
                                                  nullptr);
            if (!window)
            {
                glfwTerminate();
                return MakeError("glfwCreateWindow failed");
            }

            return std::unique_ptr<Window>(std::make_unique<WindowGLFW>(window, desc.title));
        }
    } // namespace detail
} // namespace vrf

#endif // VRF_WINDOW_GLFW
