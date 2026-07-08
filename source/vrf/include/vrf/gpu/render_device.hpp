/*
 * render_device.hpp - RAII owner of a VriDevice plus its cached interface tables.
 *
 * This is the fuller C++ layer VRI's own vri.hpp deliberately stubs out: it runs
 * the device bring-up sequence (vriCreateDevice -> vriGetInterface x2 ->
 * GetDeviceDesc -> GetQueue), caches the core + swapchain function tables, and
 * hands them to the swapchain / frame / builder helpers.
 */
#pragma once

#include <cstdint>

#include <vri/vri.h>

#include "vrf/core/result.hpp"

namespace vrf
{
    enum class GraphicsApi
    {
        Auto, // let VRI pick the best available backend
        Vulkan,
        WebGPU,
        OpenGL,
        D3D12,
        Metal
    };

    struct RenderDeviceDesc
    {
        GraphicsApi api             = GraphicsApi::Auto;
        bool        validation      = true;
        uint64_t    enabledFeatures = 0;       // bitset of VriFeatureBits (best-effort)
        const void* nativeDisplay   = nullptr; // from Window::NativeDisplay(); needed by native GLES on Wayland
    };

    class RenderDevice
    {
    public:
        RenderDevice() = default;
        ~RenderDevice();

        RenderDevice(const RenderDevice&)            = delete;
        RenderDevice& operator=(const RenderDevice&) = delete;
        RenderDevice(RenderDevice&& other) noexcept;
        RenderDevice& operator=(RenderDevice&& other) noexcept;

        [[nodiscard]] static Expected<RenderDevice> Create(const RenderDeviceDesc& desc);

        [[nodiscard]] VriDevice*                   Handle() const noexcept { return m_device; }
        [[nodiscard]] const VriCoreInterface&      Core() const noexcept { return m_core; }
        [[nodiscard]] const VriSwapChainInterface& Swap() const noexcept { return m_swap; }
        [[nodiscard]] VriQueue*                    GraphicsQueue() const noexcept { return m_graphicsQueue; }
        [[nodiscard]] const VriDeviceDesc*         Desc() const noexcept { return m_desc; }
        [[nodiscard]] VriGraphicsAPI               Api() const noexcept { return m_api; }
        [[nodiscard]] const char*                  ApiName() const noexcept;

        [[nodiscard]] explicit operator bool() const noexcept { return m_device != nullptr; }

        void WaitIdle() const;

    private:
        RenderDevice(VriDevice*                   device,
                     const VriCoreInterface&      core,
                     const VriSwapChainInterface& swap,
                     VriQueue*                    graphicsQueue,
                     const VriDeviceDesc*         desc,
                     VriGraphicsAPI               api) noexcept;

        void Reset() noexcept;

        VriDevice*            m_device        = nullptr;
        VriCoreInterface      m_core          = {};
        VriSwapChainInterface m_swap          = {};
        VriQueue*             m_graphicsQueue = nullptr;
        const VriDeviceDesc*  m_desc          = nullptr;
        VriGraphicsAPI        m_api           = VriGraphicsAPI_None;
    };
} // namespace vrf
