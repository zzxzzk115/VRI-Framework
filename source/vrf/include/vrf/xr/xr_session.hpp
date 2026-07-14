/*
 * xr_session.hpp - a running OpenXR session as a StereoRig.
 *
 * Created lazily (e.g. from an "Enter VR" button) on a device that was created
 * XR-compatible via XrSystem::VulkanCreateHooks(); destroying it leaves the
 * device intact, so VR can be entered/exited repeatedly.
 *
 * One arraySize=2 stereo swapchain; each image is wrapped as a state-tracked
 * fg::Texture, so the framegraph renders into the XR target exactly like any
 * imported texture. BeginFrame pumps xrPollEvent, paces on xrWaitFrame, and
 * locates the per-eye views; EndFrame releases the image and submits the
 * projection layer.
 *
 * Only available with vrf_with_openxr (VRF_WITH_OPENXR).
 */
#pragma once

#if defined(VRF_WITH_OPENXR)

#include <memory>

#include "vrf/xr/stereo_rig.hpp"
#include "vrf/xr/xr_system.hpp"

typedef struct XrSession_T* XrSession_Handle_Fwd; // avoid clashing with the class name below

namespace vrf
{
    class RenderDevice;
}

namespace vrf::xr
{
    enum class SessionState
    {
        Idle,     // created, runtime not yet READY
        Running,  // between xrBeginSession and xrEndSession
        Stopping, // runtime asked to stop
        Exiting,  // session/instance lost or exit requested - destroy the rig
    };

    // Raw handles for app-side OpenXR extensions (eye tracking, hand tracking,
    // ...) layered on the session without vrf having to wrap every extension.
    struct XrSessionHandles
    {
        ::XrInstance instance {nullptr};
        void*        session {nullptr};        // XrSession
        void*        appSpace {nullptr};       // XrSpace (LOCAL reference space)
        int64_t      predictedDisplayTime {0}; // XrTime of the frame between Begin/EndFrame
    };

    class XrSession final : public StereoRig
    {
    public:
        ~XrSession() override;

        XrSession(const XrSession&)            = delete;
        XrSession& operator=(const XrSession&) = delete;

        // The device MUST have been created with the system's VulkanCreateHooks.
        [[nodiscard]] static Expected<std::unique_ptr<XrSession>> Create(XrSystem&, RenderDevice&);

        [[nodiscard]] Expected<StereoFrame> BeginFrame() override;
        void                                PreSubmit(VriCommandBuffer*, StereoFrame&) override;
        void                                EndFrame(const StereoFrame&) override;

        [[nodiscard]] Extent2D  EyeExtent() const override;
        [[nodiscard]] VriFormat ColorFormat() const override;
        [[nodiscard]] bool      IsXr() const override { return true; }
        [[nodiscard]] bool      IsRunning() const override;

        [[nodiscard]] SessionState State() const;

        // Politely ask the runtime to end the session ("Exit VR" button); the
        // state machine then walks Stopping -> Exiting.
        void RequestExit();

        [[nodiscard]] XrSessionHandles Handles() const;

    private:
        struct Impl;
        explicit XrSession(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> m_impl;
    };
} // namespace vrf::xr

#endif // VRF_WITH_OPENXR
