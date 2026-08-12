/*
 * asset_cache.hpp - prebaked model cache.
 *
 * Parsing a source model is dominated by work whose result never changes: image decode,
 * mip generation, accessor decode, transform baking, tangent generation. This bakes the
 * finished `vrf::Mesh` - vertex streams, indices, submeshes, materials and textures with
 * their mip chains - into one blittable file next to the source, and reads it back on the
 * next run. A cold load of Intel Sponza + two extra models goes from minutes to the time
 * it takes to read the file.
 *
 * Deliberately narrow, unlike a general asset pipeline: there is no UUID registry, no pack
 * container, no virtual filesystem, no importer plugin surface. The only thing this format
 * knows how to represent is what vrf's own loaders already produce, and the only consumer
 * is LoadModelCached().
 *
 * The cache is a pure accelerator: deleting a .vrfcache file is always safe, and any
 * mismatch (format version, loader version, or a changed source file) silently falls back
 * to the real loader and re-bakes.
 */
#pragma once

#include <string>
#include <string_view>

#include "vrf/asset/loaders/gltf_loader.hpp"
#include "vrf/asset/mesh.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    struct AssetCacheOptions
    {
        // Off entirely: LoadModelCached degrades to a plain loader call.
        bool enabled = true;
        // Bake on a miss. Turn off for read-only asset trees or to measure a cold load.
        bool write = true;
        // Baked file path. Empty means "<modelPath>.vrfcache".
        std::string cachePath;
    };

    // Load a model through the bake cache.
    //
    // Hit  -> reads the baked mesh, skipping the source parser entirely.
    // Miss -> LoadGltf/LoadObj as usual, then (if `write`) bakes the result for next time.
    //
    // A bake failure is not a load failure: `out` is still valid and the error is logged.
    [[nodiscard]] Expected<void> LoadModelCached(std::string_view          path,
                                                 Mesh&                     out,
                                                 const GltfImportOptions&  options = {},
                                                 const AssetCacheOptions&  cache   = {});

    // Lower-level halves, exposed for tests and for tools that bake ahead of time.
    [[nodiscard]] Expected<void> WriteBakedMesh(std::string_view cachePath,
                                                std::string_view sourcePath,
                                                const Mesh&      mesh);
    // Fails (rather than returning stale data) when the header, the format/loader version or
    // the recorded source stamp does not match what is on disk now.
    [[nodiscard]] Expected<void> ReadBakedMesh(std::string_view cachePath,
                                               std::string_view sourcePath,
                                               Mesh&            out);
} // namespace vrf
