/*
 * window_backends.hpp - internal factory hooks for the window backends. Each
 * backend .cpp is compiled only when its xmake option is enabled and defines its
 * VRF_WINDOW_* macro.
 */
#pragma once

#include <memory>

#include "vrf/platform/window.hpp"

namespace vrf::detail
{
#if defined(VRF_WINDOW_SDL3)
    Expected<std::unique_ptr<Window>> CreateWindowSDL3(const WindowDesc& desc);
    // platformHandle is an SDL_WindowID, which is what ImGui_ImplSDL3 stores (the native
    // HWND/NSWindow goes in PlatformHandleRaw instead).
    Expected<VriWindowHandle> ForeignWindowHandleSDL3(void* platformHandle);
#endif
#if defined(VRF_WINDOW_GLFW)
    Expected<std::unique_ptr<Window>> CreateWindowGLFW(const WindowDesc& desc);
    // platformHandle is a GLFWwindow*, which is what ImGui_ImplGlfw stores.
    Expected<VriWindowHandle> ForeignWindowHandleGLFW(void* platformHandle);
#endif
} // namespace vrf::detail
