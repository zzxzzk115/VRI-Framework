/*
 * Derived asset cache: generated tangents and shared, content-addressed textures.
 * Source geometry and materials are parsed each load. Texture hits bypass image
 * decoding, mip generation and BC7 encoding. FullMesh retains the legacy baked
 * mesh path for callers that prefer minimum warm-load latency over disk space.
 */
#pragma once

#include <string>
#include <string_view>

#include "vrf/asset/loaders/gltf_loader.hpp"
#include "vrf/asset/mesh.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    enum class AssetCacheMode
    {
        Derived,
        FullMesh
    };

    struct AssetCacheStats
    {
        uint32_t textureHits   = 0;
        uint32_t textureMisses = 0;
        bool     tangentHit    = false;
        uint64_t bytesWritten  = 0;
    };

    struct AssetCacheOptions
    {
        // Off entirely: LoadModelCached degrades to a plain loader call.
        bool enabled = true;
        // Write missing entries. Existing entries can still be read when false.
        bool write = true;
        // FullMesh file path. Empty means "<modelPath>.vrfcache".
        std::string    cachePath;
        AssetCacheMode mode = AssetCacheMode::Derived;
        // Shared derived-cache root. Empty uses the platform user cache directory.
        // Identical textures share entries across model paths and build directories.
        std::string directory;
        // Optional per-call counters; reset by LoadModelCached. Do not share between concurrent calls.
        AssetCacheStats* stats = nullptr;
    };

    // Load a model through the bake cache.
    //
    // Derived -> load original geometry/materials and reuse generated data by content key.
    // FullMesh -> read/write the complete baked mesh, skipping the parser on a hit.
    // Cache failures are misses; they do not invalidate a successfully loaded model.
    [[nodiscard]] Expected<void> LoadModelCached(std::string_view         path,
                                                 Mesh&                    out,
                                                 const GltfImportOptions& options = {},
                                                 const AssetCacheOptions& cache   = {});

    // Lower-level halves, exposed for tests and for tools that bake ahead of time.
    [[nodiscard]] Expected<void>
    WriteBakedMesh(std::string_view cachePath, std::string_view sourcePath, const Mesh& mesh);
    // Fails (rather than returning stale data) when the header, the format/loader version or
    // the recorded source stamp does not match what is on disk now.
    [[nodiscard]] Expected<void> ReadBakedMesh(std::string_view cachePath, std::string_view sourcePath, Mesh& out);
} // namespace vrf
