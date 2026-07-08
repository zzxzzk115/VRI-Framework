#include "vrf/gpu/render_device.hpp"

#include <utility>

#include "vrf/core/log.hpp"

namespace vrf
{
    namespace
    {
        VriGraphicsAPI ToVriApi(GraphicsApi api)
        {
            switch (api)
            {
                case GraphicsApi::Vulkan:
                    return VriGraphicsAPI_Vulkan;
                case GraphicsApi::WebGPU:
                    return VriGraphicsAPI_WebGPU;
                case GraphicsApi::OpenGL:
                    return VriGraphicsAPI_OpenGL;
                case GraphicsApi::D3D12:
                    return VriGraphicsAPI_D3D12;
                case GraphicsApi::Metal:
                    return VriGraphicsAPI_Metal;
                case GraphicsApi::Auto:
                default:
                    return VriGraphicsAPI_Auto;
            }
        }

        // Single diagnostic sink for every backend + VRI validation message. Routed into vrf::Log
        // so an embedding app can redirect it via SetLogSink. Stateless, so one static suffices; its
        // address must outlive the device (VriDeviceCreationDesc keeps the pointer).
        void VRI_CALL MessageCallback(void* /*userArg*/, VriMessageSeverity severity, const char* message)
        {
            const LogLevel level = severity == VriMessageSeverity_Error   ? LogLevel::Error :
                                   severity == VriMessageSeverity_Warning ? LogLevel::Warning :
                                                                            LogLevel::Info;
            Log(level, message ? message : "");
        }
    } // namespace

    RenderDevice::RenderDevice(VriDevice*                   device,
                               const VriCoreInterface&      core,
                               const VriSwapChainInterface& swap,
                               VriQueue*                    graphicsQueue,
                               const VriDeviceDesc*         desc,
                               VriGraphicsAPI               api) noexcept :
        m_device(device), m_core(core), m_swap(swap), m_graphicsQueue(graphicsQueue), m_desc(desc), m_api(api)
    {}

    RenderDevice::RenderDevice(RenderDevice&& other) noexcept :
        m_device(std::exchange(other.m_device, nullptr)), m_core(other.m_core), m_swap(other.m_swap),
        m_graphicsQueue(std::exchange(other.m_graphicsQueue, nullptr)), m_desc(std::exchange(other.m_desc, nullptr)),
        m_api(std::exchange(other.m_api, VriGraphicsAPI_None))
    {}

    RenderDevice& RenderDevice::operator=(RenderDevice&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device        = std::exchange(other.m_device, nullptr);
            m_core          = other.m_core;
            m_swap          = other.m_swap;
            m_graphicsQueue = std::exchange(other.m_graphicsQueue, nullptr);
            m_desc          = std::exchange(other.m_desc, nullptr);
            m_api           = std::exchange(other.m_api, VriGraphicsAPI_None);
        }
        return *this;
    }

    RenderDevice::~RenderDevice() { Reset(); }

    void RenderDevice::Reset() noexcept
    {
        if (m_device)
        {
            vriDestroyDevice(m_device);
            m_device = nullptr;
        }
    }

    void RenderDevice::WaitIdle() const
    {
        if (m_device && m_core.DeviceWaitIdle)
            m_core.DeviceWaitIdle(m_device);
    }

    const char* RenderDevice::ApiName() const noexcept
    {
        switch (m_api)
        {
            case VriGraphicsAPI_Vulkan:
                return "Vulkan";
            case VriGraphicsAPI_WebGPU:
                return "WebGPU";
            case VriGraphicsAPI_OpenGL:
                return "OpenGL";
            case VriGraphicsAPI_OpenGLES:
                return "OpenGL ES";
            case VriGraphicsAPI_D3D12:
                return "D3D12";
            case VriGraphicsAPI_Metal:
                return "Metal";
            default:
                return "Unknown";
        }
    }

    Expected<RenderDevice> RenderDevice::Create(const RenderDeviceDesc& desc)
    {
        static VriCallbackInterface s_callback = [] {
            VriCallbackInterface cb {};
            cb.MessageCallback = &MessageCallback;
            return cb;
        }();

        VriDeviceCreationDesc dd {};
        dd.graphicsAPI       = ToVriApi(desc.api);
        dd.enableValidation  = desc.validation ? VRI_TRUE : VRI_FALSE;
        dd.bestEffort        = VRI_TRUE;
        dd.enabledFeatures   = desc.enabledFeatures;
        dd.nativeDisplay     = desc.nativeDisplay;
        dd.callbackInterface = &s_callback;

        VriDevice* device = nullptr;
        VriResult  r      = vriCreateDevice(&dd, &device);
        if (r != VriResult_Success)
            return MakeError(r, "RenderDevice::Create", "vriCreateDevice failed");

        VriCoreInterface core {};
        r = vriGetInterface(device, VRI_INTERFACE_CORE, sizeof(core), &core);
        if (r != VriResult_Success)
        {
            vriDestroyDevice(device);
            return MakeError(r, "RenderDevice::Create", "vriGetInterface(CORE) failed");
        }

        VriSwapChainInterface swap {};
        r = vriGetInterface(device, VRI_INTERFACE_SWAPCHAIN, sizeof(swap), &swap);
        if (r != VriResult_Success)
        {
            vriDestroyDevice(device);
            return MakeError(r, "RenderDevice::Create", "vriGetInterface(SWAPCHAIN) failed");
        }

        const VriDeviceDesc* deviceDesc = core.GetDeviceDesc(device);

        VriQueue* graphicsQueue = nullptr;
        r                       = core.GetQueue(device, VriQueueType_Graphics, 0, &graphicsQueue);
        if (r != VriResult_Success || !graphicsQueue)
        {
            vriDestroyDevice(device);
            return MakeError(r, "RenderDevice::Create", "GetQueue(Graphics) failed");
        }

        return RenderDevice(device, core, swap, graphicsQueue, deviceDesc, deviceDesc->graphicsAPI);
    }
} // namespace vrf
