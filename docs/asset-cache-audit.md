# Asset cache audit and first fixes

## Status

Windows release compilation passed. Execution of `vrf-tests.exe` is blocked by
Windows `Access is denied`, including an escalated retry. No load-time, memory,
disk-size improvement, or image-equivalence measurement is claimed. This change
is a candidate, not a validated performance release.

## Current pipeline

`vrf_bake_bc7` defaults to true. The cache uses libktx to encode UASTC and transcode
to BC7 on CPU, with a memory-bounded pool across textures. Loading KTX2 and baking
BC7 are separate options. The current bake does not request OpenCL or create an
RHI GPU device. The glTF loader uses tinygltf/stb to decode images and CPU box
filtering for mipmaps; enabling the KTX2 loader does not convert glTF PNG inputs
into a GPU encoder path.

The v1 cache stores the complete imported mesh, materials and texture mip chains.
This skips source parsing, accessor conversion, transform baking, tangent and mip
generation on a hit. It is not a derived-data-only cache. BC7 payloads and baked
geometry are derived data, but unchanged source-ready payloads are not referenced
externally. Distinct model caches do not share texture payloads. A cache file can
therefore increase disk use substantially; no fixed ratio applies.

SourceStamp recursively scans the model's parent directory, excluding filenames
containing `.vrfcache`. A sibling change can invalidate an otherwise unchanged
model. A derived-data-only redesign needs an explicit dependency manifest, import
options and encoder version in its identity, references to unchanged source-ready
data, and shared texture entries. It must distinguish source bytes from decoded
pixels: omitting a PNG's decoded/BC7 result reintroduces conversion on each launch.

## Changes

- On Windows replace an existing cache with MoveFileExW(REPLACE_EXISTING), rather
  than a rename that fails when the destination exists. Preserve the failure
  reason. This addresses repeated rebaking after source changes.
- Transfer tinygltf's RGBA image allocation into the texture instead of copying
  it and retaining both allocations. Release converted non-RGBA image storage.
- Build mipmaps using the base-level allocation and reserve the exact chain
  size, including thin textures; preserve the existing rounded byte box filter.
- Disable image decoding for texture-free imports. Bypass the v1 model cache for
  these requests because v1 does not record import options; never return textures
  from an incompatible cache or replace a full cache with a partial one.
- Log CPU wall time separately for lookup, source load, tangents, BC7 and write;
  also report parse/image-decode versus conversion/mip time, geometry/texture
  payload bytes, encoder workers, and compression-disabled builds.

The on-disk v1 representation and encoding quality settings are unchanged. This
patch does not implement shared texture storage or remove source-ready payloads;
the disk-footprint part of the request remains open.

## Reproduction and verification

From the repository root, MSVC 2022 / x64 / release:

```powershell
xmake f -y -m release --vrf_build_examples=n --vrf_with_openxr=n --vrf_with_imgui=n --vrf_window_sdl3=n --vrf_window_glfw=n
xmake build -y vrf-tests
xmake run vrf-tests
git diff --check
```

Build and diff checks pass. The run command fails before entering tests with
Permission denied. The configuration enables Vulkan, disables other RHI
backends, and enables BC7. No Vulkan or D3D12 rendering was run. Cross-backend
PSNR/FLIP and source-versus-cache image comparisons are **not measured**.

Added CPU cases check base-image byte preservation, rounded mip filtering,
replacement of an existing cache, texture-free cache isolation, and skipping
invalid image decoding for geometry-only loads. They compile but have not run.

Before merging: run these cases, then measure source/no-cache, cold-cache and
warm-cache loading on representative real assets. Save the new stage logs and
geometry/texture bytes, peak process memory, and actual cache/source file sizes.
Run identical Vulkan/D3D12 captures before and after with the same asset and
configuration. Do not infer speedup from removed copies or change BC7 quality to
improve build-time numbers.
