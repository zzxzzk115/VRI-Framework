/*
 * xr_system.hpp - OpenXR instance/system probe, run BEFORE RenderDevice::Create.
 *
 * XR_KHR_vulkan_enable2 requires the XR runtime to interpose Vulkan
 * instance/device creation and to pick the VkPhysicalDevice, so XR
 * compatibility is decided when the VriDevice is created and cannot be
 * retrofitted. The intended startup flow for a runtime-toggleable VR app:
 *
 *   auto xrSystem = vrf::xr::XrSystem::Probe();          // cheap; fails cleanly with no runtime/HMD
 *   vrf::RenderDeviceDesc dd;
 *   if (xrSystem) dd.nativeCreateInfo = xrSystem->VulkanCreateHooks();
 *   auto device = vrf::RenderDevice::Create(dd);         // XR-compatible when probed, plain otherwise
 *   ...later, on the "Enter VR" button: XrSession::Create(*xrSystem, device)
 *
 * Only available with vrf_with_openxr (VRF_WITH_OPENXR).
 */
#pragma once

#if defined(VRF_WITH_OPENXR)

#include <memory>
#include <string>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"

typedef struct XrInstance_T* XrInstance; // NOLINT: OpenXR handle fwd-decls (openxr.h defines the same)
typedef uint64_t             XrSystemId;

namespace vrf::xr
{
    struct XrSystemDesc
    {
        std::string applicationName {"vrf-app"};
    };

    class XrSystem
    {
    public:
        ~XrSystem();

        XrSystem(const XrSystem&)            = delete;
        XrSystem& operator=(const XrSystem&) = delete;
        XrSystem(XrSystem&& other) noexcept;
        XrSystem& operator=(XrSystem&& other) noexcept;

        // Create the XrInstance, find an HMD system, and resolve the enable2
        // entry points. Fails cleanly (Expected error) when no runtime or no
        // HMD is available - callers fall back to SimStereoRig.
        [[nodiscard]] static Expected<XrSystem> Probe(const XrSystemDesc& = {});

        // Feed to RenderDeviceDesc::nativeCreateInfo (a VriVulkanCreateHooks*).
        // Stable for this object's lifetime; the XrSystem must outlive device
        // creation.
        [[nodiscard]] const void* VulkanCreateHooks() const;

        [[nodiscard]] Extent2D RecommendedEyeExtent() const;

        [[nodiscard]] XrInstance Instance() const;
        [[nodiscard]] XrSystemId SystemId() const;

        [[nodiscard]] const std::string& SystemName() const;

    private:
        struct Impl;
        explicit XrSystem(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> m_impl;

        friend class XrSession;
    };
} // namespace vrf::xr

#endif // VRF_WITH_OPENXR
