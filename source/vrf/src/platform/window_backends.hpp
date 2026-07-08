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
#endif
#if defined(VRF_WINDOW_GLFW)
    Expected<std::unique_ptr<Window>> CreateWindowGLFW(const WindowDesc& desc);
#endif
} // namespace vrf::detail
