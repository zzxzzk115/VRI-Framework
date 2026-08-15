/*
 * sim_stereo_rig.hpp - desktop stereo emulation: same StereoRig interface as a
 * real XR session, but the head pose is app-driven (mouse/WASD camera) and the
 * 2-layer eye target is an ordinary owned texture the app composites into its
 * window (e.g. blit one eye, or side-by-side).
 */
#pragma once

#include "vrf/gpu/vri_types.hpp"

#include <memory>

#include "vrf/xr/stereo_rig.hpp"

namespace vrf
{
    class RenderDevice;
}

namespace vrf::xr
{
    struct SimStereoRigDesc
    {
        Extent2D  eyeExtent {1440, 1600}; // per-eye resolution (Quest-2-ish default)
        VriFormat colorFormat {VriFormat_RGBA8_UNORM};
        float     ipdMeters {0.063f};
        Fov       fov {Fov::Symmetric(glm::radians(45.0f), glm::radians(45.0f))};
        float     nearZ {0.05f};
        float     farZ {100.0f};
        // Extra usage on the eye target beyond ColorAttachment (the app usually
        // samples it to composite into the window).
        VriTextureUsageFlags extraUsage {VriTextureUsage_ShaderResource};
    };

    class SimStereoRig final : public StereoRig
    {
    public:
        [[nodiscard]] static Expected<std::unique_ptr<SimStereoRig>> Create(RenderDevice&,
                                                                            const SimStereoRigDesc& = {});

        [[nodiscard]] Expected<StereoFrame> BeginFrame() override;
        void                                PreSubmit(CommandBufferHandle*, StereoFrame&) override {}
        void                                EndFrame(const StereoFrame&) override {}

        [[nodiscard]] Extent2D  EyeExtent() const override { return m_desc.eyeExtent; }
        [[nodiscard]] VriFormat ColorFormat() const override { return m_desc.colorFormat; }
        [[nodiscard]] bool      IsXr() const override { return false; }

        // App-driven head pose (a fly camera, a recorded trajectory, ...).
        void                      SetHeadPose(const Pose& pose) { m_headPose = pose; }
        [[nodiscard]] const Pose& HeadPose() const { return m_headPose; }

        void                SetIpd(float meters) { m_desc.ipdMeters = meters; }
        [[nodiscard]] float Ipd() const { return m_desc.ipdMeters; }

        // The persistent eye target (also handed out via StereoFrame::colorTarget).
        [[nodiscard]] fg::Texture& EyeTarget() { return m_target; }

    private:
        SimStereoRig(const SimStereoRigDesc& desc, fg::Texture&& target) : m_desc {desc}, m_target {std::move(target)}
        {}

        SimStereoRigDesc m_desc;
        fg::Texture      m_target;
        Pose             m_headPose {};
    };
} // namespace vrf::xr
