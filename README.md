# VRI-Framework

[![CI](https://github.com/zzxzzk115/VRI-Framework/actions/workflows/ci.yml/badge.svg)](https://github.com/zzxzzk115/VRI-Framework/actions/workflows/ci.yml)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**High-level rendering capabilities over [VRI](https://github.com/zzxzzk115/VRI), a
cross-API Render Hardware Interface.**

VRI-Framework (`vrf`) is the reusable layer between a raw RHI and a renderer. It turns
VRI's C ABI into a modern C++23 toolkit — RAII resources, a framegraph, GPU profiling,
multi-queue submission, a bindless descriptor table, a ray-tracing stack, and an OpenXR
lifecycle — that an application or engine composes into **its own** renderer.

> **It is not a renderer and not a game engine.** `vrf` ships no frame pipeline, scene
> graph, material runtime, or shading model. It provides the building blocks; *you* own
> the passes, the scene, and the shaders. Think of it as *Vulkan + VMA + a framegraph
> library*, unified behind one clean C++ API and portable across VRI's backends.

Errors surface as `vrf::Expected<T>` (`std::expected<T, Error>`) — no exceptions in the
API surface — and every GPU object is move-only RAII.

---

## Capabilities

### Core
- `Expected<T>` / `Error` result type and `MakeError` helpers — one consistent error model.
- Settable log sink (route into spdlog, a file, …), glm math aliases, `Extent2D`.
- CPU profiling zones (`VRF_ZONE`) that compile to nothing without Tracy.

### Platform
- Abstract `Window` with **SDL3** and **GLFW** backends, yielding a `VriWindowHandle`.

### GPU — RHI capabilities
- **Device & presentation** — `RenderDevice` (graphics + **compute/transfer queues**),
  `Swapchain`, single-shot `Frame` and N-frames-in-flight `FrameStream` (timeline-synced).
- **RAII handles** — `Unique<T>` owns raw `Vri*` resources (`UniqueBuffer`/`Texture`/
  `Descriptor`/`Pipeline`/…); implicit `T*` conversion for VRI calls, freed on scope exit.
- **Fluent builders** — `GraphicsPipelineBuilder`, `ComputePipelineBuilder`,
  `PipelineLayoutBuilder`, `BufferBuilder`, `TextureBuilder`, with backend-agnostic
  `ShaderVariants` (SPIR-V / WGSL / D3D12) so pipelines aren't locked to one API.
- **Shaders** — `ShaderLibrary` resolves offline-cooked `.vshlib` variants; attribute-driven
  `GpuVertexLayout` selects the matching `VTX_HAS_*` permutation from a mesh's layout.
- **Descriptor table** — `DescriptorTable`: an indexed set that uses true hardware **bindless**
  (runtime-sized, partially-bound array) when the device supports it, and a fixed-capacity
  array otherwise — same API and shader either way.
- **Caches** — a generic desc-keyed `ResourceCache` of RAII handles (keyed by value, not by a
  hash, so no collision aliasing) and `SamplerCache` on top of it.
- **GPU profiling** — `GpuProfiler`: nestable `VRF_GPU_ZONE(cmd, name)` timestamp zones with
  per-frame readback; no-op on backends without timestamp queries.
- **Ray tracing** — `Blas`/`Tlas` (split create/build for per-frame TLAS rebuilds),
  `RayTracingPipeline` + builder (owns the SBT), over VRI's ray-tracing extension.
- **Uploads & readback** — `UploadMesh`/`UploadTexture`, `ImmediateSubmit`, `ReadbackTexture`
  / `SaveTextureToPng`.
- **UI** — a native `Hud` (stb_easy_font, no ImGui dependency) for in-game / VR overlays, plus
  an optional `ImGuiBackend` for host-driven frame loops.

### Framegraph (`fg`)
- Resource types (`fg::Texture`/`fg::Buffer`) that track their own `VriAccessLayoutStage` and
  emit explicit barriers, driven by packed per-pass access declarations.
- `RenderContext` — dynamic rendering, descriptor-set allocation/binding, multiview/stereo.
- `TransientResources` — desc-keyed pooling with idle eviction; `importTexture` brings external
  (swapchain / XR / history) targets into a frame's graph.

### XR / VR
- `StereoRig` interface with two implementations: `SimStereoRig` (desktop emulation) and
  `XrSession` (a live OpenXR session as a 2-layer stereo target).
- `VrDriver` — a single façade over the whole OpenXR lifecycle (runtime probe, device-creation
  hooks, session enter/exit, sim fallback), so app code never touches OpenXR types.
- OpenXR is opt-in (`vrf_with_openxr`); without it, stereo modes run on the simulator rig.

### Assets & loaders
- Loader-agnostic SoA `Mesh`, a multi-shading-model `Material` (`std::variant` over Unlit /
  PBR-MetallicRoughness / PBR-SpecularGlossiness / Phong), `Texture`, `Light`, `GaussianSplat`.
- Raw loaders (file → data, third-party headers confined to `.cpp`): `LoadObj`, `LoadGltf`,
  `LoadImage` (png/jpg/hdr), `LoadKtxDds` (DDS + KTX1), opt-in `LoadKtx2` (libktx).
- **3D Gaussian Splatting** via the vendored **GaussForge** (+ Niantic spz) stack — `LoadGaussianSplat`
  dispatches by extension across `.ply` / `.compressed.ply` / `.spz` / `.splat` / `.ksplat`
  (the `.sog` WebP-container format is trimmed out, matching the reference build).

### App
- `Application` — a batteries-included window + present loop (used by the examples).
- `RenderModule` — the embeddable render layer (device + swapchain + frame + depth) driven by a
  host engine's own window and loop; owns neither the window nor the loop.

---

## Quick start

```cpp
#include <vrf/vrf.hpp>

auto device = vrf::RenderDevice::Create({.api = vrf::GraphicsApi::Vulkan});
if (!device) return fail(device.error().message);

vrf::UniqueBuffer vbo {*device, *vrf::BufferBuilder{}
    .SetSize(size).SetUsage(VriBufferUsage_VertexBuffer)
    .SetMemoryLocation(VriMemoryLocation_HostUpload).Build(*device)};
// ...freed automatically; no manual Destroy.
```

See `examples/` for complete programs (triangle, deferred framegraph, ray tracing, compute,
OpenXR, gaussian splatting).

## Building

Requires [xmake](https://xmake.io). Dependencies (including the `vri` package) resolve from the
author's xmake-repo, already wired in `xmake.lua`.

```sh
xmake f -c                       # configure (Vulkan backend by default)
xmake build                      # vrf static library + examples + tests
xmake run vrf_example_triangle
```

### Options

| Option | Default | Description |
|---|---|---|
| `vrf_window_sdl3` / `vrf_window_glfw` | `true` | Window backends |
| `vrf_backend_vulkan` | `true` | Reference VRI backend |
| `vrf_backend_gl` / `_wgpu` / `_d3d12` / `_metal` | `false` | Opt-in VRI backends |
| `vrf_with_openxr` | `false` | OpenXR (VR/stereo) support — Vulkan only |
| `vrf_with_imgui` | `true` | Dear ImGui backend |
| `vrf_with_tracy` | `false` | Tracy CPU profiling zones |
| `vrf_loader_ktx2` | `false` | KTX2 texture loader (libktx; heavy) |
| `vrf_loader_draco` | `false` | Draco-compressed glTF |
| `vrf_cook_shaders` | `false` | Re-cook `.vshlib` shader variants via `vshaderc` |
| `vrf_build_examples` / `vrf_build_tests` | `true` | Build examples / tests |

Backend and window are also selectable at runtime:

```sh
VRI_API=vulkan  xmake run vrf_example_model_viewer path/to/model.gltf   # VRI's backend env var
VRF_WINDOW=glfw xmake run vrf_example_model_viewer                      # window backend
```

## Examples

`triangle` · `rt_triangle` · `xr_triangle` · `model_viewer` (OBJ/glTF) · `framegraph_deferred` ·
`compute_saxpy` · `compute_rayquery` · `rt_inpaint` · `gaussian_splat` · `gaussian_rt`
(run as `vrf_example_<name>`).

## Layout

```
source/vrf/include/vrf/
  core/       Expected/Error, logging, glm math, profiling zones
  platform/   Window (SDL3 + GLFW)
  gpu/        RenderDevice, Swapchain, Frame/FrameStream, RenderModule, RAII handles,
              builders/, DescriptorTable, SamplerCache/ResourceCache, GpuProfiler,
              ray tracing, HUD, ImGui backend, uploads, screenshot, shader library
  fg/         framegraph — resource state tracking, RenderContext, transient pooling
  xr/         StereoRig (sim + OpenXR), VrDriver, stereo math
  asset/      mesh, material, texture, light, gaussian_splat, loaders/
  app/        Application (RenderModule lives in gpu/)
examples/     runnable samples (Slang shaders)
tests/        doctest suite (GPU cases self-skip without a device)
```

## License

MIT — see [LICENSE](LICENSE).
