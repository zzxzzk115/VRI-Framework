/*
 * vr_driver.hpp - the single stereo/VR façade an app renders against.
 *
 * Encapsulates the whole OpenXR lifecycle so app code never touches OpenXR
 * types (XrSystem/XrSession) or the VRF_WITH_OPENXR macro: the pre-device
 * runtime probe, the desktop SimStereoRig fallback, the lazily-created
 * XrSession, device-creation hooks, session-state reconciliation, and the
 * internal draining an enter/exit needs. Builds and runs sim-only when OpenXR
 * is unavailable or disabled.
 *
 * Startup (VR must interpose Vulkan device creation, so it is two-phase):
 *
 *   auto vr = vrf::xr::VrDriver::Probe({.applicationName = "app"});   // before the device
 *   vrf::RenderDeviceDesc dd;
 *   dd.nativeCreateInfo = vr.DeviceCreateHooks();                     // null unless a HMD was probed
 *   auto device = vrf::RenderDevice::Create(dd);
 *   ... create the FrameStream ...
 *   vr.Initialize(device, frames, {.eyeExtent = ...});               // after the device
 *
 * Per frame:
 *   vr.Poll();                                 // reconcile a self-exited session
 *   auto frame = vr.ActiveRig().BeginFrame();  // XrSession while in VR, else the sim rig
 *
 * "Enter VR" -> vr.EnterVr(); "Exit VR" -> vr.RequestExitVr().
 */
#pragma once

#include <memory>
#include <string>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"
#include "vrf/xr/sim_stereo_rig.hpp"
#include "vrf/xr/stereo_rig.hpp"

namespace vrf
{
    class RenderDevice;
    class FrameStream;
} // namespace vrf

namespace vrf::xr
{
    struct VrDriverDesc
    {
        std::string applicationName {"vrf-app"};
        bool        enableXr {true}; // false = never probe OpenXR (e.g. a --sim flag)
    };

    class VrDriver
    {
    public:
        VrDriver(); // inert / sim-only until Probe()+Initialize()
        ~VrDriver();

        VrDriver(const VrDriver&)            = delete;
        VrDriver& operator=(const VrDriver&) = delete;
        VrDriver(VrDriver&&) noexcept;
        VrDriver& operator=(VrDriver&&) noexcept;

        // Phase 1, BEFORE RenderDevice::Create: probe the OpenXR runtime. Always
        // returns a usable driver - it falls back to sim-only when OpenXR is
        // disabled, not built, or has no runtime/HMD.
        [[nodiscard]] static VrDriver Probe(const VrDriverDesc& = {});

        // Feed to RenderDeviceDesc::nativeCreateInfo. Null unless a headset was
        // probed; the driver must outlive device creation.
        [[nodiscard]] const void* DeviceCreateHooks() const;

        // Phase 2, AFTER the device: bind services and create the simulator rig.
        // `simDesc.eyeExtent` is replaced by the runtime's recommended per-eye
        // extent when a headset was probed.
        [[nodiscard]] Expected<void> Initialize(RenderDevice&, FrameStream&, SimStereoRigDesc = {});

        // Recreate the simulator rig at a new desc (e.g. a resolution-profile
        // change). Drains in-flight frames internally.
        [[nodiscard]] Expected<void> ResizeSim(const SimStereoRigDesc&);

        // Enter VR: create the XrSession (drains first). No-op without a runtime
        // or when already in VR.
        [[nodiscard]] Expected<void> EnterVr();

        // Leave VR immediately: drain + destroy the session (falls back to sim).
        void ExitVr();

        // Politely ask the runtime to end the session; Poll() tears it down once
        // the runtime finishes stopping.
        void RequestExitVr();

        // Call once per frame before BeginFrame: destroy a session that exited on
        // its own (headset menu / RequestExitVr), falling back to the sim rig.
        void Poll();

        [[nodiscard]] StereoRig&    ActiveRig();         // XrSession while in VR, else the sim rig
        [[nodiscard]] SimStereoRig& Sim();               // the desktop rig (pose driving, extent)
        [[nodiscard]] bool          VrAvailable() const; // an OpenXR runtime + HMD was probed
        [[nodiscard]] bool          VrActive() const;    // an XrSession is created and running

        [[nodiscard]] const std::string& SystemName() const;           // "" when unavailable
        [[nodiscard]] Extent2D           RecommendedEyeExtent() const; // {0,0} when unavailable

        // Which runtime answered the probe. Provenance for anything captured
        // from the rig: the same headset hands out a different frustum under a
        // different runtime. "" / 0 when unavailable.
        [[nodiscard]] const std::string& RuntimeName() const;
        [[nodiscard]] const std::string& RuntimeVersion() const;
        [[nodiscard]] uint32_t           VendorId() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace vrf::xr
