#if defined(VRF_WITH_OPENXR)

#define XR_USE_GRAPHICS_API_VULKAN
// clang-format off
// Order matters: vulkan.h must precede openxr_platform.h, which uses
// VkInstance/VkDevice/... in the OpenXR<->Vulkan structs (types only, no calls).
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// clang-format on

#include "vrf/xr/xr_system.hpp"

#include <cstring>
#include <vector>

#include <vri/vri.h>
#include <vri/ext/vri_ext_interop.h>

#include "vrf/core/log.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vrf::xr
{
    namespace
    {
        [[nodiscard]] PFN_vkGetInstanceProcAddr loadVulkanGipa()
        {
#if defined(_WIN32)
            HMODULE m = LoadLibraryA("vulkan-1.dll");
            return m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr")) :
                       nullptr;
#elif defined(__APPLE__)
            void* m = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
            if (!m)
                m = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
            return m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m, "vkGetInstanceProcAddr")) : nullptr;
#else
            void* m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
            return m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m, "vkGetInstanceProcAddr")) : nullptr;
#endif
        }
    } // namespace

    struct XrSystem::Impl
    {
        ::XrInstance instance {XR_NULL_HANDLE};
        ::XrSystemId systemId {XR_NULL_SYSTEM_ID};
        std::string  systemName;
        Extent2D     recommendedEyeExtent {};

        PFN_xrCreateVulkanInstanceKHR     createInstance {nullptr};
        PFN_xrGetVulkanGraphicsDevice2KHR getDevice {nullptr};
        PFN_xrCreateVulkanDeviceKHR       createDevice {nullptr};
        PFN_vkGetInstanceProcAddr         gipa {nullptr};
        VriVulkanCreateHooks              hooks {};

        ~Impl()
        {
            if (instance != XR_NULL_HANDLE)
            {
                xrDestroyInstance(instance);
            }
        }

        // VRI builds the VkInstanceCreateInfo and calls this; forward it through
        // xrCreateVulkanInstanceKHR so the runtime adds its extensions and
        // interposes creation, then resolve the physical device OpenXR mandates.
        static int32_t VRI_CALL HookCreateInstance(void* userData, const void* vkCreateInfo, void* outInstance)
        {
            auto* self = static_cast<Impl*>(userData);

            XrVulkanInstanceCreateInfoKHR ci {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
            ci.systemId               = self->systemId;
            ci.pfnGetInstanceProcAddr = self->gipa;
            ci.vulkanCreateInfo       = static_cast<const VkInstanceCreateInfo*>(vkCreateInfo);

            VkResult   vkr  = VK_ERROR_INITIALIZATION_FAILED;
            VkInstance inst = VK_NULL_HANDLE;
            if (XR_FAILED(self->createInstance(self->instance, &ci, &inst, &vkr)) || vkr != VK_SUCCESS)
            {
                return vkr != VK_SUCCESS ? vkr : VK_ERROR_INITIALIZATION_FAILED;
            }
            *static_cast<VkInstance*>(outInstance) = inst;

            VkPhysicalDevice                 physical = VK_NULL_HANDLE;
            XrVulkanGraphicsDeviceGetInfoKHR gi {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
            gi.systemId       = self->systemId;
            gi.vulkanInstance = inst;
            self->getDevice(self->instance, &gi, &physical);
            self->hooks.physicalDevice = physical;
            return VK_SUCCESS;
        }

        static int32_t VRI_CALL HookCreateDevice(void*       userData,
                                                 void*       physicalDevice,
                                                 const void* vkCreateInfo,
                                                 void*       outDevice)
        {
            auto* self = static_cast<Impl*>(userData);

            XrVulkanDeviceCreateInfoKHR ci {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
            ci.systemId               = self->systemId;
            ci.pfnGetInstanceProcAddr = self->gipa;
            ci.vulkanPhysicalDevice   = static_cast<VkPhysicalDevice>(physicalDevice);
            ci.vulkanCreateInfo       = static_cast<const VkDeviceCreateInfo*>(vkCreateInfo);

            VkResult vkr = VK_ERROR_INITIALIZATION_FAILED;
            VkDevice dev = VK_NULL_HANDLE;
            if (XR_FAILED(self->createDevice(self->instance, &ci, &dev, &vkr)) || vkr != VK_SUCCESS)
            {
                return vkr != VK_SUCCESS ? vkr : VK_ERROR_INITIALIZATION_FAILED;
            }
            *static_cast<VkDevice*>(outDevice) = dev;
            return VK_SUCCESS;
        }
    };

    XrSystem::XrSystem(std::unique_ptr<Impl> impl) noexcept : m_impl {std::move(impl)} {}
    XrSystem::~XrSystem()                              = default;
    XrSystem::XrSystem(XrSystem&&) noexcept            = default;
    XrSystem& XrSystem::operator=(XrSystem&&) noexcept = default;

    Expected<XrSystem> XrSystem::Probe(const XrSystemDesc& desc)
    {
        auto impl = std::make_unique<Impl>();

        // Instance with XR_KHR_vulkan_enable2 (the form runtimes like the Meta
        // XR Simulator require).
        {
            const char*          exts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
            XrInstanceCreateInfo ci {XR_TYPE_INSTANCE_CREATE_INFO};
            std::strncpy(ci.applicationInfo.applicationName,
                         desc.applicationName.c_str(),
                         XR_MAX_APPLICATION_NAME_SIZE - 1);
            ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
            ci.enabledExtensionCount      = 1;
            ci.enabledExtensionNames      = exts;
            if (XR_FAILED(xrCreateInstance(&ci, &impl->instance)))
            {
                return MakeError("xr::XrSystem::Probe: no OpenXR runtime available");
            }
        }

        {
            XrSystemGetInfo gi {XR_TYPE_SYSTEM_GET_INFO};
            gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            if (XR_FAILED(xrGetSystem(impl->instance, &gi, &impl->systemId)))
            {
                return MakeError("xr::XrSystem::Probe: no HMD system (headset not connected?)");
            }
        }

        {
            XrSystemProperties props {XR_TYPE_SYSTEM_PROPERTIES};
            if (XR_SUCCEEDED(xrGetSystemProperties(impl->instance, impl->systemId, &props)))
            {
                impl->systemName = props.systemName;
            }
        }

        // enable2 entry points are extension functions - resolve dynamically.
        auto load = [&](const char* name, auto& fp) {
            xrGetInstanceProcAddr(impl->instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fp));
        };
        PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
        load("xrGetVulkanGraphicsRequirements2KHR", getRequirements);
        load("xrCreateVulkanInstanceKHR", impl->createInstance);
        load("xrGetVulkanGraphicsDevice2KHR", impl->getDevice);
        load("xrCreateVulkanDeviceKHR", impl->createDevice);
        if (!getRequirements || !impl->createInstance || !impl->getDevice || !impl->createDevice)
        {
            return MakeError("xr::XrSystem::Probe: runtime lacks XR_KHR_vulkan_enable2 entry points");
        }

        // Spec requires querying graphics requirements before device creation.
        XrGraphicsRequirementsVulkanKHR requirements {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        getRequirements(impl->instance, impl->systemId, &requirements);

        impl->gipa = loadVulkanGipa();
        if (!impl->gipa)
        {
            return MakeError("xr::XrSystem::Probe: could not load the Vulkan loader");
        }

        // Recommended per-eye extent from the stereo view configuration.
        {
            uint32_t viewCount = 0;
            xrEnumerateViewConfigurationViews(
                impl->instance, impl->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
            std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            xrEnumerateViewConfigurationViews(impl->instance,
                                              impl->systemId,
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              viewCount,
                                              &viewCount,
                                              views.data());
            if (viewCount >= 1)
            {
                impl->recommendedEyeExtent = {views[0].recommendedImageRectWidth,
                                              views[0].recommendedImageRectHeight};
            }
        }

        impl->hooks.api            = VriGraphicsAPI_Vulkan;
        impl->hooks.createInstance = &Impl::HookCreateInstance;
        impl->hooks.createDevice   = &Impl::HookCreateDevice;
        impl->hooks.userData       = impl.get();

        LogInfo("xr: OpenXR system \"{}\" ({}x{} per eye)",
                impl->systemName,
                impl->recommendedEyeExtent.width,
                impl->recommendedEyeExtent.height);
        return XrSystem {std::move(impl)};
    }

    const void* XrSystem::VulkanCreateHooks() const { return &m_impl->hooks; }

    Extent2D XrSystem::RecommendedEyeExtent() const { return m_impl->recommendedEyeExtent; }

    ::XrInstance XrSystem::Instance() const { return m_impl->instance; }
    ::XrSystemId XrSystem::SystemId() const { return m_impl->systemId; }

    const std::string& XrSystem::SystemName() const { return m_impl->systemName; }
} // namespace vrf::xr

#endif // VRF_WITH_OPENXR
