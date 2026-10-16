# Offline Gaussian splat capture

Captured on 2026-10-16 in a newly created independent workspace. This PR adds an offline entry to the existing Gaussian splat example, using VRF's offscreen texture and PNG readback APIs.

```sh
xmake f -y --vrf_build_tests=n --vrf_window_glfw=n --vrf_with_imgui=n
xmake build -y -j4 vrf_example_gaussian_splat
DISPLAY= WAYLAND_DISPLAY= VRF_SPLAT_OFFLINE=1 \
  VRF_SPLAT_CAPTURE_ANGLE=2.1 VRF_SPLAT_CAPTURE_HEIGHT=0.65 \
  VRF_SPLAT_CAPTURE_RADIUS=1.8 VRF_EXAMPLE_SCREENSHOT=/tmp/vrf-splat.png \
  xmake run vrf_example_gaussian_splat /path/to/vasset/resources/splats/hornedlizard.spz
```

This renders one frame without creating a window or swapchain. The output is 1280×720; camera aspect ratio, focal length and Gaussian pixel radii use the actual offscreen target extent. Converting this PNG to WebP preserves its dimensions and adds no framing or retouching.

The chosen camera has an orbit angle of 2.1 radians, height 0.65 and horizontal radius 1.8. These optional settings apply only when `VRF_EXAMPLE_SCREENSHOT` is supplied. Invalid values fall back to the normal defaults; nonpositive radii are rejected. Normal windowed operation remains available when `VRF_SPLAT_OFFLINE` is absent. Offline mode requires an output path.

Input: [`hornedlizard.spz` from vasset](https://github.com/zzxzzk115/vasset/blob/fb50e451a98fae6d678719cca0f49f9e5688a79b/resources/splats/hornedlizard.spz). Obtain that existing fixture separately; it is not duplicated here. The loader reports 786,233 splats with SH degree 3.

Validation: native build; offline render with display environment variables cleared; windowed automatic-exit regression; 1280×720 output inspection. The existing CPU/GPU depth-bucket comparison reports nonzero inversions, so the render is a visual example rather than an exhaustive sorting correctness result.

Capture comparison: expanding the frustum-center culling margin to include essentially all forward-facing splats changed the high-view result by only 0.156/255 mean absolute RGB intensity. EWA versus 3DGUT at the lower camera view differed by 0.215/255. Neither adjustment materially filled the peripheral background. The final image uses EWA and the lower camera; these observations do not establish whether remaining peripheral artifacts originate in the input reconstruction or the renderer.
