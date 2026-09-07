#include "vrf/xr/vr_driver.hpp"

#include <optional>

#include "vrf/core/log.hpp"
#include "vrf/gpu/frame_stream.hpp"
#include "vrf/gpu/render_device.hpp"

#if defined(VRF_WITH_OPENXR)
#include "vrf/xr/xr_session.hpp"
#include "vrf/xr/xr_system.hpp"
#endif

namespace vrf::xr
{
    struct VrDriver::Impl
    {
#if defined(VRF_WITH_OPENXR)
        std::optional<XrSystem>    system;
        std::unique_ptr<XrSession> session;
#endif
        std::unique_ptr<SimStereoRig> sim;
        RenderDevice*                 device {nullptr};
        FrameStream*                  frames {nullptr};
        std::string                   systemName;  // cached so SystemName() is valid without a session
        std::string                   runtimeName; // ditto: probe-time identity outlives the session
        std::string                   runtimeVersion;
        uint32_t                      vendorId {0};
        Extent2D                      recommendedExtent; // {0,0} unless a headset was probed

        // Drain in-flight frames before recreating a rig / tearing down a session.
        void drain()
        {
            if (frames)
            {
                frames->WaitAll();
            }
            if (device)
            {
                device->WaitIdle();
            }
        }
    };

    VrDriver::VrDriver() : m_impl {std::make_unique<Impl>()} {}
    VrDriver::~VrDriver()                              = default;
    VrDriver::VrDriver(VrDriver&&) noexcept            = default;
    VrDriver& VrDriver::operator=(VrDriver&&) noexcept = default;

    VrDriver VrDriver::Probe(const VrDriverDesc& desc)
    {
        VrDriver driver;
#if defined(VRF_WITH_OPENXR)
        if (desc.enableXr)
        {
            auto probed = XrSystem::Probe({.applicationName = desc.applicationName});
            if (probed)
            {
                driver.m_impl->system.emplace(std::move(*probed));
                driver.m_impl->systemName        = driver.m_impl->system->SystemName();
                driver.m_impl->runtimeName       = driver.m_impl->system->RuntimeName();
                driver.m_impl->runtimeVersion    = driver.m_impl->system->RuntimeVersion();
                driver.m_impl->vendorId          = driver.m_impl->system->VendorId();
                driver.m_impl->recommendedExtent = driver.m_impl->system->RecommendedEyeExtent();
                LogInfo("vrf::xr::VrDriver: OpenXR runtime active - system '{}', recommended eye {}x{}",
                        driver.m_impl->systemName,
                        driver.m_impl->recommendedExtent.width,
                        driver.m_impl->recommendedExtent.height);
            }
            else
            {
                LogInfo("vrf::xr::VrDriver: {} - VR unavailable, using the simulator rig", probed.error().message);
            }
        }
#else
        // Silent fallback here cost an afternoon: with vrf_with_openxr=n the whole probe compiles
        // away, so "--mode vr without --sim" ran the DESKTOP rig and looked like the headset/Meta XR
        // Simulator had simply not been picked up. Nothing distinguished it from a failed probe
        // because nothing was logged at all. Say it, and say the flag that fixes it.
        if (desc.enableXr)
        {
            LogWarning("vrf::xr::VrDriver: built without OpenXR (vrf_with_openxr=n) - falling back to the "
                       "desktop simulator rig; rebuild with `xmake f --vrf_with_openxr=y` for a real session");
        }
        (void)desc;
#endif
        return driver;
    }

    const void* VrDriver::DeviceCreateHooks() const
    {
#if defined(VRF_WITH_OPENXR)
        if (m_impl->system)
        {
            return m_impl->system->VulkanCreateHooks();
        }
#endif
        return nullptr;
    }

    Expected<void> VrDriver::Initialize(RenderDevice& device, FrameStream& frames, SimStereoRigDesc simDesc)
    {
        m_impl->device = &device;
        m_impl->frames = &frames;
#if defined(VRF_WITH_OPENXR)
        // A real headset dictates the per-eye render resolution.
        if (m_impl->system && m_impl->system->RecommendedEyeExtent().width > 0)
        {
            simDesc.eyeExtent = m_impl->system->RecommendedEyeExtent();
        }
#endif
        auto rig = SimStereoRig::Create(device, simDesc);
        if (!rig)
        {
            return std::unexpected(rig.error());
        }
        m_impl->sim = std::move(*rig);
        return {};
    }

    Expected<void> VrDriver::ResizeSim(const SimStereoRigDesc& simDesc)
    {
        if (!m_impl->device)
        {
            return MakeError("VrDriver::ResizeSim called before Initialize");
        }
        m_impl->drain();
        auto rig = SimStereoRig::Create(*m_impl->device, simDesc);
        if (!rig)
        {
            return std::unexpected(rig.error());
        }
        m_impl->sim = std::move(*rig);
        return {};
    }

    Expected<void> VrDriver::EnterVr()
    {
#if defined(VRF_WITH_OPENXR)
        if (!m_impl->system)
        {
            return MakeError("VrDriver::EnterVr: no OpenXR runtime");
        }
        if (!m_impl->device)
        {
            return MakeError("VrDriver::EnterVr called before Initialize");
        }
        if (m_impl->session)
        {
            return {}; // already in VR
        }
        m_impl->drain();
        auto session = XrSession::Create(*m_impl->system, *m_impl->device);
        if (!session)
        {
            return std::unexpected(session.error());
        }
        m_impl->session = std::move(*session);
        return {};
#else
        return MakeError("VrDriver::EnterVr: built without OpenXR");
#endif
    }

    void VrDriver::ExitVr()
    {
#if defined(VRF_WITH_OPENXR)
        if (m_impl->session)
        {
            m_impl->drain();
            m_impl->session.reset();
        }
#endif
    }

    void VrDriver::RequestExitVr()
    {
#if defined(VRF_WITH_OPENXR)
        if (m_impl->session)
        {
            m_impl->session->RequestExit();
        }
#endif
    }

    void VrDriver::Poll()
    {
#if defined(VRF_WITH_OPENXR)
        // A session can exit on its own (headset menu, the runtime quitting the app, or a prior
        // RequestExit finishing). That ends VR, NOT the process: the app drops back to the desktop
        // simulator rig and keeps running, and Enter VR is live again.
        if (m_impl->session && !m_impl->session->IsRunning())
        {
            LogInfo("vrf::xr::VrDriver: session ended - back on the simulator rig");
            ExitVr();
        }
#endif
    }

    StereoRig& VrDriver::ActiveRig()
    {
#if defined(VRF_WITH_OPENXR)
        if (m_impl->session)
        {
            return *m_impl->session;
        }
#endif
        return *m_impl->sim;
    }

    SimStereoRig& VrDriver::Sim() { return *m_impl->sim; }

    bool VrDriver::VrAvailable() const
    {
#if defined(VRF_WITH_OPENXR)
        return m_impl->system.has_value();
#else
        return false;
#endif
    }

    bool VrDriver::VrActive() const
    {
#if defined(VRF_WITH_OPENXR)
        return m_impl->session != nullptr;
#else
        return false;
#endif
    }

    const std::string& VrDriver::SystemName() const { return m_impl->systemName; }

    Extent2D VrDriver::RecommendedEyeExtent() const { return m_impl->recommendedExtent; }

    const std::string& VrDriver::RuntimeName() const { return m_impl->runtimeName; }
    const std::string& VrDriver::RuntimeVersion() const { return m_impl->runtimeVersion; }
    uint32_t           VrDriver::VendorId() const { return m_impl->vendorId; }
} // namespace vrf::xr
