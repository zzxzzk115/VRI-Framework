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
        // Returns false when the backend reported the swapchain out of date / suboptimal at
        // present time (the image may not have reached the screen). Acquire alone does NOT
        // surface this on all drivers - a caller that ignores it can wedge on a black window
        // after aggressive resizes; treat false like AcquireResult::outOfDate (rebuild).
        bool Present();
        // Rebuild the swapchain at `extent`. Returns false when the surface can't back a
        // swapchain right now (zero extent / minimized / mid-resize transition) - the OLD
        // swapchain and Extent() bookkeeping are kept untouched so the caller's
        // "window != swapchain" retry keeps firing until a rebuild succeeds. (Committing the
        // extent on a failed rebuild wedged apps in a black-screen acquire-outOfDate loop:
        // the sizes looked equal, so nothing ever retried.)
        bool Resize(Extent2D extent);
        // Switch the present mode (Fifo = vsync, Immediate/Mailbox = uncapped). No backend can
        // retune a live swapchain, so this REBUILDS it: idle the device first, and rebuild
        // anything holding the old textures (borrowed framegraph textures) afterwards, exactly
        // as for a Resize. Returns false if the rebuild failed, in which case the previous
        // swapchain stays usable and the mode is unchanged. A no-op (and true) if `mode` is
        // already active.
        bool SetPresentMode(VriPresentMode mode);

        [[nodiscard]] VriTexture*    Texture(uint32_t index) const;
        [[nodiscard]] uint32_t       TextureCount() const noexcept { return static_cast<uint32_t>(m_textures.size()); }
        [[nodiscard]] VriFormat      Format() const noexcept { return m_format; }
        [[nodiscard]] Extent2D       Extent() const noexcept { return m_extent; }
        [[nodiscard]] VriPresentMode PresentMode() const noexcept { return m_desc.presentMode; }
        [[nodiscard]] VriSwapChain*  Handle() const noexcept { return m_swapchain; }

    private:
        void Reset() noexcept;
        void RefreshTextures();
        // Set m_extent to the backend's ACTUAL swapchain size (window systems may override the
        // requested one); falls back to `requested` on backends without GetSwapChainExtent.
        void AdoptActualExtent(Extent2D requested);

        RenderDevice* m_device    = nullptr;
        VriSwapChain* m_swapchain = nullptr;
        VriFormat     m_format    = VriFormat_Unknown;
        Extent2D      m_extent {};
        // The desc this swapchain was built from, kept so SetPresentMode can rebuild with
        // everything else (window, format, texture count) unchanged.
        SwapchainDesc            m_desc {};
        std::vector<VriTexture*> m_textures;
    };
} // namespace vrf
