#pragma once

#include "vrf/asset/asset_cache.hpp"
#include <filesystem>
#include <optional>
#include <span>

namespace vrf::detail
{
    // Internal loader context: image keys are formed from the encoded source bytes,
    // before stb decoding, so a hit avoids decode, mip filtering and BC7 encoding.
    struct DerivedCache
    {
        std::filesystem::path               directory;
        bool                                write;
        AssetCacheStats*                    stats;
        std::vector<std::string>            imageKeys;
        std::vector<std::optional<Texture>> images;

        explicit DerivedCache(const AssetCacheOptions& options);
        std::string            ImageKey(std::span<const unsigned char> encoded) const;
        std::optional<Texture> ReadTexture(const std::string& key) const;
        void                   FinishTexture(const std::string& key, Texture& texture) const;
        void                   Tangents(Mesh& mesh) const;
    };

    Expected<void>
    LoadGltfCachedTextures(std::string_view path, Mesh& out, const GltfImportOptions& options, DerivedCache& cache);
} // namespace vrf::detail
