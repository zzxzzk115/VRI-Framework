#include "asset/derived_cache.hpp"
#include "asset/texture_compress.hpp"
#include "vrf/core/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace vrf::detail
{
    namespace
    {
        using Bytes  = std::vector<uint8_t>;
        namespace fs = std::filesystem;

        std::string Hex(XXH128_hash_t hash)
        {
            constexpr char digits[] = "0123456789abcdef";
            std::string    out(32, '0');
            for (unsigned i = 0; i < 16; ++i)
            {
                out[15 - i] = digits[(hash.high64 >> (i * 4)) & 15];
                out[31 - i] = digits[(hash.low64 >> (i * 4)) & 15];
            }
            return out;
        }

        void Put(Bytes& out, uint64_t value, unsigned bytes = 4)
        {
            for (unsigned i = 0; i < bytes; ++i)
                out.push_back(static_cast<uint8_t>(value >> (i * 8)));
        }

        struct Cursor
        {
            std::span<const uint8_t> data;
            bool                     ok = true;
            uint64_t                 Get(unsigned bytes = 4)
            {
                if (data.size() < bytes)
                {
                    ok = false;
                    return 0;
                }
                uint64_t value = 0;
                for (unsigned i = 0; i < bytes; ++i)
                    value |= uint64_t(data[i]) << (i * 8);
                data = data.subspan(bytes);
                return value;
            }
        };

        fs::path DefaultDirectory()
        {
            const auto env = [](const char* name) {
                const char* value = std::getenv(name);
                return value && *value ? fs::path(value) : fs::path();
            };
#if defined(_WIN32)
            if (auto p = env("LOCALAPPDATA"); !p.empty())
                return p / "vrf" / "cache";
#else
            if (auto p = env("XDG_CACHE_HOME"); !p.empty() && p.is_absolute())
                return p / "vrf";
            if (auto p = env("HOME"); !p.empty())
            {
#if defined(__APPLE__)
                return p / "Library" / "Caches" / "vrf";
#else
                return p / ".cache" / "vrf";
#endif
            }
#endif
            // A caller without a user directory can still load assets, without disk caching.
            return {};
        }

        std::optional<Bytes> ReadBlob(const fs::path& path)
        {
            std::error_code ec;
            const uint64_t  size = fs::file_size(path, ec);
            if (ec || size < 32 || size - 32 > std::numeric_limits<size_t>::max())
                return {};
            std::ifstream in(path, std::ios::binary);
            uint8_t       header[32];
            if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "VRFDRV01", 8) != 0)
                return {};
            Cursor     cursor {std::span(header).subspan(8)};
            const auto length = cursor.Get(8);
            const auto low = cursor.Get(8), high = cursor.Get(8);
            if (length != size - 32)
                return {};
            Bytes bytes;
            try
            {
                bytes.resize(static_cast<size_t>(length));
            }
            catch (const std::bad_alloc&)
            {
                return {};
            }
            if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length)))
                return {};
            const auto hash = XXH3_128bits(bytes.data(), bytes.size());
            if (hash.low64 != low || hash.high64 != high)
                return {};
            return bytes;
        }

        uint64_t WriteBlob(const fs::path& path, std::span<const uint8_t> bytes)
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            if (ec)
                return 0;
            // Exclusive staging directory prevents two writers from sharing a temporary file.
            static std::atomic<uint64_t> sequence {0};
            fs::path                     staging;
            bool                         created = false;
            for (int attempt = 0; attempt < 16 && !created; ++attempt)
            {
                staging = path.string() + ".tmp-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                          std::to_string(sequence.fetch_add(1));
                created = fs::create_directory(staging, ec);
                if (ec)
                    return 0;
            }
            if (!created)
                return 0;
            struct Cleanup
            {
                fs::path path;
                ~Cleanup()
                {
                    std::error_code ignored;
                    fs::remove_all(path, ignored);
                }
            } cleanup {staging};
            const fs::path temporary = staging / "entry";
            const auto     hash      = XXH3_128bits(bytes.data(), bytes.size());
            Bytes          header {'V', 'R', 'F', 'D', 'R', 'V', '0', '1'};
            Put(header, bytes.size(), 8);
            Put(header, hash.low64, 8);
            Put(header, hash.high64, 8);
            {
                std::ofstream out(temporary, std::ios::binary);
                out.write(reinterpret_cast<const char*>(header.data()), header.size());
                out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                out.flush();
                if (!out)
                    return 0;
            }
            fs::rename(temporary, path, ec);
#if defined(_WIN32)
            std::error_code fileEc;
            if (ec && fs::is_regular_file(path, fileEc))
            {
                // Windows rename cannot replace a file. A reader in this short gap sees a miss.
                fs::remove(path, ec);
                fs::rename(temporary, path, ec);
            }
#endif
            return ec ? 0 : bytes.size() + header.size();
        }

        fs::path Entry(const fs::path& directory, const char* kind, const std::string& key)
        {
            return directory / "derived-v1" / kind / key.substr(0, 2) / (key + ".bin");
        }
    } // namespace

    DerivedCache::DerivedCache(const AssetCacheOptions& options) :
        directory(options.directory.empty() ? DefaultDirectory() : fs::path(options.directory)), write(options.write),
        stats(options.stats)
    {}

    std::string DerivedCache::ImageKey(std::span<const unsigned char> encoded) const
    {
        XXH3_state_t state;
        XXH3_128bits_reset(&state);
        // Includes the mip/filter/import recipe and encoder, not the image's path or name.
        constexpr char recipe[] = "vrf-gltf-rgba8-box-mips-v1";
        XXH3_128bits_update(&state, recipe, sizeof(recipe));
        XXH3_128bits_update(&state, Bc7EncoderName(), std::strlen(Bc7EncoderName()));
        XXH3_128bits_update(&state, encoded.data(), encoded.size());
        return Hex(XXH3_128bits_digest(&state));
    }

    std::optional<Texture> DerivedCache::ReadTexture(const std::string& key) const
    {
        if (directory.empty() || key.empty())
            return {};
        auto bytes = ReadBlob(Entry(directory, "textures", key));
        if (!bytes)
            return {};
        Cursor  c {*bytes};
        Texture t;
        t.width      = static_cast<uint32_t>(c.Get());
        t.height     = static_cast<uint32_t>(c.Get());
        t.mipLevels  = static_cast<uint32_t>(c.Get());
        t.format     = static_cast<VriFormat>(c.Get());
        t.compressed = t.format == VriFormat_BC7_UNORM;
        if (!t.width || !t.height || !t.mipLevels || t.mipLevels > 32 ||
            (t.format != VriFormat_RGBA8_UNORM && t.format != VriFormat_BC7_UNORM))
            return {};
        uint64_t offset = 0;
        for (uint32_t mip = 0; mip < t.mipLevels; ++mip)
        {
            TextureSubresource sub;
            sub.mipLevel            = mip;
            sub.width               = std::max(t.width >> mip, 1u);
            sub.height              = std::max(t.height >> mip, 1u);
            sub.offset              = offset;
            sub.size                = c.Get(8);
            const uint64_t expected = t.compressed ?
                                          ((uint64_t(sub.width) + 3) / 4) * ((uint64_t(sub.height) + 3) / 4) * 16 :
                                          uint64_t(sub.width) * sub.height * 4;
            if (!c.ok || sub.size != expected || sub.size > bytes->size() - offset)
                return {};
            offset += sub.size;
            t.subresources.push_back(sub);
        }
        if (!c.ok || offset != c.data.size())
            return {};
        t.data.assign(c.data.begin(), c.data.end());
        return t;
    }

    void DerivedCache::FinishTexture(const std::string& key, Texture& texture) const
    {
        CompressTextureBc7(texture);
        if (!write || directory.empty() || key.empty() || texture.data.empty())
            return;
        Bytes bytes;
        bytes.reserve(16 + texture.subresources.size() * 8 + texture.data.size());
        Put(bytes, texture.width);
        Put(bytes, texture.height);
        Put(bytes, texture.mipLevels);
        Put(bytes, texture.format);
        for (const auto& sub : texture.subresources)
            Put(bytes, sub.size, 8);
        bytes.insert(bytes.end(), texture.data.begin(), texture.data.end());
        const auto written = WriteBlob(Entry(directory, "textures", key), bytes);
        if (stats)
            stats->bytesWritten += written;
    }

    void DerivedCache::Tangents(Mesh& mesh) const
    {
        if (mesh.positions.empty() || mesh.tangents.size() == mesh.positions.size() ||
            mesh.normals.size() != mesh.positions.size() || mesh.texCoords0.size() != mesh.positions.size())
            return;
        XXH3_state_t state;
        XXH3_128bits_reset(&state);
        constexpr char recipe[] = "vrf-tangents-lengyel-v1";
        XXH3_128bits_update(&state, recipe, sizeof(recipe));
        const auto hashArray = [&](const auto& values) {
            const uint64_t count = values.size();
            XXH3_128bits_update(&state, &count, sizeof(count));
            XXH3_128bits_update(&state, values.data(), values.size() * sizeof(values[0]));
        };
        hashArray(mesh.positions);
        hashArray(mesh.normals);
        hashArray(mesh.texCoords0);
        hashArray(mesh.indices);
        const auto path = Entry(directory, "tangents", Hex(XXH3_128bits_digest(&state)));
        if (!directory.empty())
        {
            if (auto bytes = ReadBlob(path); bytes && bytes->size() == mesh.positions.size() * sizeof(Tangent))
            {
                mesh.tangents.resize(mesh.positions.size());
                std::memcpy(mesh.tangents.data(), bytes->data(), bytes->size());
                mesh.attributes |= VertexAttribute::Tangent;
                if (stats)
                    stats->tangentHit = true;
                return;
            }
        }
        mesh.tangents = GenerateTangents(mesh);
        mesh.attributes |= VertexAttribute::Tangent;
        if (write && !directory.empty())
        {
            const auto written = WriteBlob(
                path, {reinterpret_cast<const uint8_t*>(mesh.tangents.data()), mesh.tangents.size() * sizeof(Tangent)});
            if (stats)
                stats->bytesWritten += written;
        }
    }
} // namespace vrf::detail
