/*
 * swapchain.hpp - RAII wrapper over a VriSwapChain (acquire / present / resize).
 */
#pragma once

#include <cstdint>
#include <vector>

#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;

    struct SwapchainDesc
    {
        VriWindowHandle window {};
        Extent2D        extent {};
        VriFormat       format      = VriFormat_BGRA8_UNORM;
        uint32_t        textureNum  = 3;
        VriPresentMode  presentMode = VriPresentMode_Fifo;
    };

    struct AcquireResult
    {
        uint32_t index     = 0;
        bool     outOfDate = false; // true => the caller should Resize() and skip this frame
    };

    class Swapchain
    {
    public:
        Swapchain() = default;
        ~Swapchain();

        Swapchain(const Swapchain&)            = delete;
        Swapchain& operator=(const Swapchain&) = delete;
        Swapchain(Swapchain&& other) noexcept;
        Swapchain& operator=(Swapchain&& other) noexcept;

        [[nodiscard]] static Expected<Swapchain> Create(RenderDevice& device, const SwapchainDesc& desc);

        [[nodiscard]] AcquireResult Acquire();
        void                        Present();
        void                        Resize(Extent2D extent);

        [[nodiscard]] VriTexture*   Texture(uint32_t index) const;
        [[nodiscard]] uint32_t      TextureCount() const noexcept { return static_cast<uint32_t>(m_textures.size()); }
        [[nodiscard]] VriFormat     Format() const noexcept { return m_format; }
        [[nodiscard]] Extent2D      Extent() const noexcept { return m_extent; }
        [[nodiscard]] VriSwapChain* Handle() const noexcept { return m_swapchain; }

    private:
        void Reset() noexcept;
        void RefreshTextures();

        RenderDevice*            m_device    = nullptr;
        VriSwapChain*            m_swapchain = nullptr;
        VriFormat                m_format    = VriFormat_Unknown;
        Extent2D                 m_extent {};
        std::vector<VriTexture*> m_textures;
    };
} // namespace vrf
