#include "vrf/platform/window.hpp"

#include "window_backends.hpp"

namespace vrf
{
    Expected<VriWindowHandle> ForeignWindowHandle(const WindowBackend backend, void* platformHandle)
    {
        switch (backend)
        {
            case WindowBackend::SDL3:
#if defined(VRF_WINDOW_SDL3)
                return detail::ForeignWindowHandleSDL3(platformHandle);
#else
                return MakeError("SDL3 window backend not compiled in (enable vrf_window_sdl3)");
#endif
            case WindowBackend::GLFW:
#if defined(VRF_WINDOW_GLFW)
                return detail::ForeignWindowHandleGLFW(platformHandle);
#else
                return MakeError("GLFW window backend not compiled in (enable vrf_window_glfw)");
#endif
            case WindowBackend::Auto:
                break;
        }
        return MakeError("ForeignWindowHandle needs a concrete backend, not Auto");
    }

    Expected<std::unique_ptr<Window>> Window::Create(const WindowDesc& desc)
    {
        WindowBackend backend = desc.backend;
        if (backend == WindowBackend::Auto)
        {
#if defined(VRF_WINDOW_SDL3)
            backend = WindowBackend::SDL3;
#elif defined(VRF_WINDOW_GLFW)
            backend = WindowBackend::GLFW;
#else
            return MakeError("No window backend compiled in (enable vrf_window_sdl3 or vrf_window_glfw)");
#endif
        }

        switch (backend)
        {
            case WindowBackend::SDL3:
#if defined(VRF_WINDOW_SDL3)
                return detail::CreateWindowSDL3(desc);
#else
                return MakeError("SDL3 window backend not compiled in (enable vrf_window_sdl3)");
#endif
            case WindowBackend::GLFW:
#if defined(VRF_WINDOW_GLFW)
                return detail::CreateWindowGLFW(desc);
#else
                return MakeError("GLFW window backend not compiled in (enable vrf_window_glfw)");
#endif
            default:
                return MakeError("Unknown window backend");
        }
    }
} // namespace vrf
