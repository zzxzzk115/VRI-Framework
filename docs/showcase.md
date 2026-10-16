# Showcase capture

Captured on 2026-10-16 in a newly created independent workspace. Source checkout: `9b4548e72a2e387b6b45eb75836a2e91b4c19bae`.

Build the actual native Gaussian splat example:

```sh
xmake f -y --vrf_build_tests=n --vrf_window_glfw=n --vrf_with_imgui=n
xmake build -y -j4 vrf_example_gaussian_splat
VRF_EXAMPLE_AUTO_EXIT=1 VRF_SPLAT_CAPTURE_ANGLE=1.5 VRF_SPLAT_CAPTURE_HEIGHT=1.8 \
  VRF_EXAMPLE_SCREENSHOT=/tmp/vrf-splat.png \
  xmake run vrf_example_gaussian_splat /path/to/vasset/resources/splats/hornedlizard.spz
```

Input: [`hornedlizard.spz` from vasset](https://github.com/zzxzzk115/vasset/blob/fb50e451a98fae6d678719cca0f49f9e5688a79b/resources/splats/hornedlizard.spz). Obtain that existing fixture separately; it is not duplicated in this PR. The loader reported 786,233 splats with SH degree 3. The example normally orbits with time. This PR adds optional capture-only angle (radians) and height settings; the image fixes them at 1.5 and 1.8. Invalid values fall back to the normal camera behavior. These settings are ignored when no screenshot path is supplied. The built-in framebuffer readback is 1280×720 and is converted to WebP without added borders or retouching.

The native run completed. Its existing CPU/GPU bucket comparison reports some nonzero inversions, so this capture is a visual example rather than a claim of exhaustive sorting correctness.
