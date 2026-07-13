/*
 * stereo_rig.hpp - the single interface app code renders stereo against.
 *
 * Two implementations: XrSession (a real OpenXR session, vrf_with_openxr) and
 * SimStereoRig (desktop-driven emulation). Both hand out a 2-layer array color
 * target per frame, so a multiview renderer is oblivious to which rig drives
 * it - "enter VR" swaps the rig, not the renderer.
 *
 * Frame protocol:
 *   auto frame = rig.BeginFrame();               // pose/timing (XR: xrWaitFrame paces here)
 *   if (frame && frame->shouldRender) {
 *       ...record rendering into frame->colorTarget...
 *       rig.PreSubmit(cmd, *frame);              // final target transition, recorded into cmd
 *   }
 *   ...submit command buffer...
 *   rig.EndFrame(*frame);                        // XR: release image + xrEndFrame
 */
#pragma once

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"
#include "vrf/fg/texture.hpp"
#include "vrf/xr/stereo.hpp"

namespace vrf::xr
{
    struct StereoFrame
    {
        bool        shouldRender {false};
        double      displayTimeSec {0.0}; // predicted display time (XR) / now (sim)
        StereoViews views;
        fg::Texture* colorTarget {nullptr}; // 2-layer array target (layer 0 = left eye)
        Extent2D     extent {};             // per-eye extent
    };

    class StereoRig
    {
    public:
        virtual ~StereoRig() = default;

        [[nodiscard]] virtual Expected<StereoFrame> BeginFrame() = 0;

        // Record the frame-final transition of colorTarget into `cmd` (before
        // the command buffer is submitted).
        virtual void PreSubmit(VriCommandBuffer* cmd, StereoFrame&) = 0;

        // After submission. XR: xrReleaseSwapchainImage + xrEndFrame.
        virtual void EndFrame(const StereoFrame&) = 0;

        [[nodiscard]] virtual Extent2D  EyeExtent() const   = 0;
        [[nodiscard]] virtual VriFormat ColorFormat() const = 0;
        [[nodiscard]] virtual bool      IsXr() const        = 0;

        // True while frames should be pumped; false = rig wants to stop
        // (session exiting / runtime loss). Sim rigs always return true.
        [[nodiscard]] virtual bool IsRunning() const { return true; }
    };
} // namespace vrf::xr
