# Derived asset cache and direct BC7 encoding

`LoadModelCached` defaults to a shared cache of generated tangents and encoded
textures. Positions, source normals, UVs, colors, indices and materials are read
from the source model each time. Authored tangents are preserved without caching
a duplicate. Generated tangents are keyed by the actual positions, normals, UVs
and indices supplied to the tangent generator.

For glTF/GLB images, the cache key hashes the encoded source image bytes and the
import/mip/encoder recipe. A hit bypasses image decoding, mip generation and
encoding. A miss decodes and bakes one image before proceeding to the next; the
loader does not retain the whole scene's decoded RGBA textures. Copies of an
image in different models or build directories use the same entry, while image
names and materials still come from the current source model. OBJ uses the
tangent cache; its loader does not currently import texture pixels.

BC7 is encoded directly with the vendored `bc7e.ispc` encoder using the `fast`
preset and SSE2/AVX2 runtime dispatch on desktop x86-64. Other targets, or
`--vrf_bake_bc7_simd=n`, use the portable `bc7enc` encoder. Block batches parallelize
a single large image without a second full RGBA copy or UASTC intermediate.
Partial blocks repeat edge texels, and all supplied mip levels are preserved.
The two encoders and the uncompressed build have distinct cache recipes.

## Location and compatibility

The default root is `$XDG_CACHE_HOME/vrf` or `$HOME/.cache/vrf` on Linux,
`$HOME/Library/Caches/vrf` on macOS, and `%LOCALAPPDATA%/vrf/cache` on Windows.
`AssetCacheOptions::directory` overrides it. The cache contains only generated
data and can be deleted when the application is not loading assets.

Files carry a format magic, length and 128-bit payload checksum. Truncation or
checksum failures cause a miss and rebuild. Writers stage in exclusive temporary
directories and rename complete entries. `write=false` allows reads and in-memory
generation, but writes no new files. `enabled=false` calls the source loader.
`loadTextures=false` skips image decoding and returns no texture array, even if
a textured cache already exists. Optional `AssetCacheStats` reports hits, misses,
tangent reuse and bytes written for one load.

Callers requiring the old whole-mesh behavior can select
`AssetCacheOptions::mode = AssetCacheMode::FullMesh` and optionally `cachePath`.
`WriteBakedMesh` and `ReadBakedMesh` remain available. The old `.vrfcache` files
are not automatically migrated or deleted. Changing to the direct encoder bumps
the full-mesh loader version; the old UASTC bake is not reused as a new bake.

Keeping BC7/mips is deliberate: removing them would put expensive image work
back into every load. This cache trades some warm-load parsing time for less
disk space. It does not impose an eviction limit; superseded content keys can
accumulate until the user clears the cache. Already compressed textures loaded
through the existing DDS/KTX loaders remain directly uploadable; they do not go
through the glTF image cache.

## Reproduce the measurements

```sh
xmake f --vrf_build_examples=n --vrf_build_benchmarks=y
xmake vrf-cache-bench
xmake vrf-bc7-bench
xmake run vrf-cache-bench path/to/scene.gltf path/to/cache-directory
xmake run vrf-cache-bench path/to/scene.gltf path/to/cache-directory
xmake run vrf-bc7-bench path/to/BaseColor.png path/to/Normal.png
```

Use an empty directory for the first cache run. The cache tool prints load time,
counts, texture bytes and a geometry hash, followed by cache counters. The BC7
tool compares the previous libktx UASTC-fastest-to-BC7 path (one libktx thread)
with direct block-parallel encoding of the same mip-0 image. It also decodes
both outputs and reports RGBA PSNR. This is a single-image comparison, not a
measurement of the old multi-image worker pool or complete scene startup.

On the local Linux x86-64 machine, three 4096x4096 Sponza textures measured:

| Texture | UASTC to BC7 (s) | Direct BC7 (s) | Previous PSNR (dB) | Direct PSNR (dB) |
| --- | ---: | ---: | ---: | ---: |
| arch stone wall base color | 3.3403 | 0.4023 | 46.361 | 47.836 |
| arch stone wall normal | 4.0734 | 0.3756 | 24.361 | 28.274 |
| arch stone wall roughness/metalness | 3.7757 | 0.3873 | 34.043 | 38.816 |

The first Sponza derived-cache load completed in 72.42 seconds, with 80 texture
misses and 1,891,835,744 bytes written. These measurements are environment-specific;
image decoding, source parsing and storage remain part of scene load time.

The previous whole-mesh cold bake took 234.17 seconds in a separate run (its
memory bound reduced the encoder pool from 20 to 3 threads). The new Sponza warm
loads took 3.19 and 3.24 seconds, with all 80 texture entries and tangents hitting
and no writes. This was not an isolated, repeated end-to-end benchmark; memory
pressure and other local work affect these timings.

For Sponza + Tree + Benk, the previous caches totalled 2,669,652,173 bytes;
the new shared cache totals 2,105,202,672 bytes (85 texture entries and 3 tangent
entries), a 21.1% reduction. There are no identical source-image keys across this
particular set, so the saving comes from not copying source geometry/materials.
Cross-build or cross-model duplicates additionally share their cache files.
All three models' positions, normals, tangents, UV0, colors and indices hash
identically to the previous baked geometry.

Validation: 39 tests / 660 assertions passed, including the Vulkan GPU cases
available on the local machine. The consuming pixelwise-viewpoint-warping app
built successfully and rendered three frames of the combined scene with all
85 texture entries hitting. Portable BC7 and compression-disabled cache tests
also passed. Encoded texture bytes change with the encoder, so captures used for
image comparisons must use the same cache recipe on both sides.
