#include "vrf/xr/sim_stereo_rig.hpp"

#include <chrono>

#include "vrf/gpu/render_device.hpp"

namespace vrf::xr
{
    Expected<std::unique_ptr<SimStereoRig>> SimStereoRig::Create(RenderDevice& device, const SimStereoRigDesc& desc)
    {
        auto target = fg::Texture::Create(device,
                                          {
                                              .extent = desc.eyeExtent,
                                              .format = desc.colorFormat,
                                              .layers = 2,
                                              .usage  = VriTextureUsage_ColorAttachment | desc.extraUsage,
                                          });
        if (!target)
        {
            return std::unexpected(target.error());
        }
        return std::unique_ptr<SimStereoRig> {new SimStereoRig {desc, std::move(*target)}};
    }

    Expected<StereoFrame> SimStereoRig::BeginFrame()
    {
        StereoFrame frame;
        frame.shouldRender = true;
        frame.displayTimeSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        frame.colorTarget = &m_target;
        frame.extent      = m_desc.eyeExtent;

        frame.views.nearZ = m_desc.nearZ;
        frame.views.farZ  = m_desc.farZ;

        // Eyes sit +-ipd/2 along the head's right axis; same orientation/fov.
        const Vec3 right = m_headPose.orientation * Vec3 {1.0f, 0.0f, 0.0f};
        for (uint32_t i = 0; i < 2; ++i)
        {
            auto& eye            = frame.views.eye[i];
            const float side     = i == 0 ? -0.5f : 0.5f;
            eye.pose.orientation = m_headPose.orientation;
            eye.pose.position    = m_headPose.position + right * (side * m_desc.ipdMeters);
            eye.fov              = m_desc.fov;
            eye.view             = ViewFromPose(eye.pose);
            eye.proj             = ProjectionFromFov(eye.fov, m_desc.nearZ, m_desc.farZ);
        }
        return frame;
    }
} // namespace vrf::xr
