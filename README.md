# VRI-Framework

A minimal, embeddable **rendering framework** built on top of
[VRI](https://github.com/zzxzzk115/VRI) (a cross-API Render Hardware Interface).
It is **not** a game engine — it is a thin, reusable toolset that a renderer or
engine can stand on:

1. **Window layer** — an abstract `vrf::Window` with **SDL3** and **GLFW**
   backends, producing a `VriWindowHandle` for the VRI swapchain.
2. **Loader-agnostic data structures** — SoA `Mesh`, a multi-shading-model
   `Material` (`std::variant` over Unlit / PBR-MetallicRoughness /
   PBR-SpecularGlossiness / Phong + lossless properties), `Texture`, `Light`, and
   `GaussianSplat`, all decoupled from any file format.
3. **Minimal raw loaders** (file → data struct; no asset-import pipeline) with all
   third-party headers confined to `.cpp` files:
   - meshes — `LoadObj` (tinyobjloader → Phong), `LoadGltf` (tinygltf → MR / spec-gloss / unlit)
   - textures — `LoadImage` (stb: png/jpg/hdr), `LoadKtxDds` (vendored dds-ktx: DDS + KTX1),
     `LoadTexture` dispatches by extension. KTX2 (`LoadKtx2`, libktx) is opt-in via
     the `vrf_loader_ktx2` option (heavy dep; off by default).
   - gaussian splats — `LoadGaussianSplatPly` (binary 3DGS PLY), `LoadGaussianSplatSplat`.
4. **CPU → GPU bridge** — `UploadMesh` / `UploadTexture` stage CPU assets into GPU
   buffers/textures.
5. **C++23 + builder pattern** — fluent `Builder`s wrapping VRI's aggregate
   descriptor structs (`GraphicsPipelineBuilder`, `PipelineLayoutBuilder`,
   `BufferBuilder`, `TextureBuilder`), with backend-agnostic `ShaderVariants`
   (SPIR-V / WGSL / D3D12) so pipelines are not locked to one API.

**Shading is developer-owned.** The framework does not ship a built-in shading
model or uber-shader — a `Material` is pure data; you bring your own pipelines and
shaders and interpret materials however your renderer wants.

**Two ways to use it:**
- `vrf::Application` — a batteries-included window + present loop (the examples).
- `vrf::RenderModule` — the embeddable render layer (device + swapchain + frame +
  depth) driven by a host engine's own window and loop; takes a native
  `VriWindowHandle`, owns neither the window nor the loop. Embed this into an
  engine as a render module / layer / server.

## Building

Requires [xmake](https://xmake.io). Dependencies (including the `vri` package)
resolve from the author's xmake-repo, already wired in `xmake.lua`.

```sh
xmake f -c            # configure (Vulkan backend by default)
xmake build           # build the vrf static library + examples + tests
xmake run vrf_example_triangle
```

### Options

| Option                 | Default | Description                                  |
|------------------------|---------|----------------------------------------------|
| `vrf_window_sdl3`      | `true`  | Build the SDL3 window backend                |
| `vrf_window_glfw`      | `true`  | Build the GLFW window backend                |
| `vrf_backend_vulkan`   | `true`  | Vulkan backend (via the `vri` package)       |
| `vrf_backend_gl/wgpu/d3d12/metal` | `false` | Opt-in additional VRI backends    |
| `vrf_loader_ktx2`      | `false` | KTX2 texture loader via libktx (heavy dep)   |
| `vrf_build_examples`   | `true`  | Build the examples                           |
| `vrf_build_tests`      | `true`  | Build the tests                              |

## Layout

```
source/vrf/          the vrf static library (public headers under include/vrf/)
  include/vrf/core       Error/Expected, logging (settable sink), glm math aliases
  include/vrf/platform   Window (SDL3 + GLFW backends)
  include/vrf/gpu        RenderDevice, Swapchain, Frame, RenderModule, upload, builders/
  include/vrf/asset      vertex, mesh, material, texture, light, gaussian_splat, loaders/
  include/vrf/app        Application (window + present loop over RenderModule)
examples/            triangle, model_viewer (loads OBJ/glTF; Slang shaders)
tests/               doctest tests
external/            dependency declarations (add_requires)
```

Run the model viewer on a model (or a procedural cube with no argument):

```sh
xmake run vrf_example_model_viewer path/to/model.gltf   # or .glb / .obj
VRF_WINDOW=glfw xmake run vrf_example_model_viewer       # pick the window backend
VRF_API=vulkan  xmake run vrf_example_model_viewer       # pick the VRI backend
```

## License

MIT — see [LICENSE](LICENSE).
