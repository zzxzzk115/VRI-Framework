#include "vrf/gpu/swapchain.hpp"

#include <utility>

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    Swapchain::Swapchain(Swapchain&& other) noexcept :
        m_device(std::exchange(other.m_device, nullptr)), m_swapchain(std::exchange(other.m_swapchain, nullptr)),
        m_format(other.m_format), m_extent(other.m_extent), m_textures(std::move(other.m_textures))
    {}

    Swapchain& Swapchain::operator=(Swapchain&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device    = std::exchange(other.m_device, nullptr);
            m_swapchain = std::exchange(other.m_swapchain, nullptr);
            m_format    = other.m_format;
            m_extent    = other.m_extent;
            m_textures  = std::move(other.m_textures);
        }
        return *this;
    }

    Swapchain::~Swapchain() { Reset(); }

    void Swapchain::Reset() noexcept
    {
        if (m_swapchain && m_device)
            m_device->Swap().DestroySwapChain(m_swapchain);
        m_swapchain = nullptr;
        m_textures.clear();
    }

    void Swapchain::RefreshTextures()
    {
        VriTexture* textures[8] = {};
        uint32_t    count       = 8;
        m_device->Swap().GetSwapChainTextures(m_swapchain, textures, &count);
        m_textures.assign(textures, textures + count);
    }

    void Swapchain::AdoptActualExtent(const Extent2D requested)
    {
        // The window system dictates the real swapchain size (Vulkan surface currentExtent);
        // during rapid resizes the caller's window metrics lag behind it. Extent() feeds render
        // areas/viewports, so it must reflect the ACTUAL backbuffer - a render area larger than
        // the attachment is undefined behavior (GPU faults -> black window).
        m_extent = requested;
        if (m_device->Swap().GetSwapChainExtent != nullptr)
        {
            uint32_t width = 0, height = 0;
            if (m_device->Swap().GetSwapChainExtent(m_swapchain, &width, &height) == VriResult_Success &&
                width != 0 && height != 0)
            {
                m_extent = {width, height};
            }
        }
    }

    Expected<Swapchain> Swapchain::Create(RenderDevice& device, const SwapchainDesc& desc)
    {
        VriSwapChainDesc scd {};
        scd.window      = desc.window;
        scd.queue       = device.GraphicsQueue();
        scd.format      = desc.format;
        scd.width       = desc.extent.width;
        scd.height      = desc.extent.height;
        scd.textureNum  = desc.textureNum;
        scd.presentMode = desc.presentMode;

        VriSwapChain*   handle = nullptr;
        const VriResult r      = device.Swap().CreateSwapChain(device.Handle(), &scd, &handle);
        if (r != VriResult_Success)
            return MakeError(r, "Swapchain::Create", "CreateSwapChain failed");

        Swapchain swapchain;
        swapchain.m_device    = &device;
        swapchain.m_swapchain = handle;
        swapchain.m_format    = desc.format;
        swapchain.AdoptActualExtent(desc.extent);
        swapchain.RefreshTextures();
        return swapchain;
    }

    AcquireResult Swapchain::Acquire()
    {
        uint32_t        index = 0;
        const VriResult r     = m_device->Swap().AcquireNextTexture(m_swapchain, nullptr, 0, &index);
        if (r == VriResult_OutOfDate)
            return {0, true};
        return {index, false};
    }

    bool Swapchain::Present() { return m_device->Swap().Present(m_swapchain, nullptr, 0) == VriResult_Success; }

    bool Swapchain::Resize(Extent2D extent)
    {
        if (extent.IsZero())
            return false;
        // Only commit new bookkeeping when the backend actually rebuilt the swapchain. On failure
        // (minimized surface / transient zero currentExtent during a resize) the backend keeps
        // the old swapchain, so the old bookkeeping must stay too.
        if (m_device->Swap().Resize(m_swapchain, extent.width, extent.height) != VriResult_Success)
            return false;
        AdoptActualExtent(extent);
        RefreshTextures();
        return true;
    }

    VriTexture* Swapchain::Texture(uint32_t index) const
    {
        return index < m_textures.size() ? m_textures[index] : nullptr;
    }
} // namespace vrf
