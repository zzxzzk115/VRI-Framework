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
        swapchain.m_extent    = desc.extent;
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

    void Swapchain::Present() { m_device->Swap().Present(m_swapchain, nullptr, 0); }

    void Swapchain::Resize(Extent2D extent)
    {
        if (extent.IsZero())
            return;
        m_device->Swap().Resize(m_swapchain, extent.width, extent.height);
        m_extent = extent;
        RefreshTextures();
    }

    VriTexture* Swapchain::Texture(uint32_t index) const
    {
        return index < m_textures.size() ? m_textures[index] : nullptr;
    }
} // namespace vrf
