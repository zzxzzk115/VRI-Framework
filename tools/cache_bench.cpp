#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vrf/asset/asset_cache.hpp>
#define XXH_INLINE_ALL
#include <xxhash.h>
int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;
    vrf::AssetCacheOptions options;
#ifdef LEGACY
    options.cachePath = std::string(argv[1]) + ".vrfcache";
#else
    vrf::AssetCacheStats stats;
    options.directory = argv[2];
    options.stats     = &stats;
#endif
    vrf::Mesh mesh;
    auto      start = std::chrono::steady_clock::now();
#ifdef LEGACY
    // Read an existing full cache without silently re-baking the user's assets.
    auto loaded = vrf::ReadBakedMesh(options.cachePath, argv[1], mesh);
#else
    auto loaded = vrf::LoadModelCached(argv[1], mesh, {}, options);
#endif
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!loaded)
    {
        std::fprintf(stderr, "%s\n", loaded.error().message.c_str());
        return 1;
    }
    XXH3_state_t state;
    XXH3_128bits_reset(&state);
    auto hash = [&](auto& v) {
        auto n = v.size();
        XXH3_128bits_update(&state, &n, sizeof(n));
        XXH3_128bits_update(&state, v.data(), v.size() * sizeof(v[0]));
    };
    hash(mesh.positions);
    hash(mesh.normals);
    hash(mesh.tangents);
    hash(mesh.texCoords0);
    hash(mesh.colors);
    hash(mesh.indices);
    auto   digest = XXH3_128bits_digest(&state);
    size_t bytes  = 0;
    for (auto& t : mesh.textures)
        bytes += t.data.size();
    std::printf("seconds=%.4f vertices=%zu tangents=%zu textures=%zu texture_bytes=%zu geometry_hash=%016llx%016llx\n",
                seconds,
                mesh.positions.size(),
                mesh.tangents.size(),
                mesh.textures.size(),
                bytes,
                (unsigned long long)digest.high64,
                (unsigned long long)digest.low64);
#ifndef LEGACY
    std::printf("texture_hits=%u texture_misses=%u tangent_hit=%d bytes_written=%llu\n",
                stats.textureHits,
                stats.textureMisses,
                stats.tangentHit,
                (unsigned long long)stats.bytesWritten);
#endif
}
