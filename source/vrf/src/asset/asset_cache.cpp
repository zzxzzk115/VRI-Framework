#include "vrf/asset/asset_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <system_error>
#include <thread>
#include <vector>

#include "vrf/asset/loaders/obj_loader.hpp"
#include "vrf/core/log.hpp"

#if defined(VRF_ENABLE_BAKE_BC7)
#include <ktx.h>
#if defined(_WIN32)
// NOMINMAX: windows.h defines min/max as macros, which breaks every std::max below.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#elif defined(__linux__)
#include <unistd.h>
#endif
#endif

namespace vrf
{
    namespace
    {
        constexpr char     kMagic[8]      = {'V', 'R', 'F', 'B', 'A', 'K', 'E', '\0'};
        constexpr uint32_t kFormatVersion = 1;
        // Bump when a loader changes what it produces from unchanged source bytes (transform
        // baking, attribute union, mip filter, ...). Existing caches then miss and re-bake
        // instead of feeding stale geometry into a changed pipeline.
        constexpr uint32_t kLoaderVersion  = 1;
        constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
        constexpr uint64_t kFnvPrime       = 1099511628211ull;

        void HashBytes(uint64_t& h, const void* data, size_t size)
        {
            const auto* p = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i)
            {
                h ^= p[i];
                h *= kFnvPrime;
            }
        }

        void HashString(uint64_t& h, std::string_view s) { HashBytes(h, s.data(), s.size()); }

        // Identity of everything the load depends on, without re-parsing any of it: the model
        // file plus every sibling file it could reference (glTF pulls .bin and images by
        // relative URI). Only names, sizes and write times are read, so this stays a stat walk
        // even for Sponza's texture folder.
        //
        // Excludes the cache's own files - it lives next to the source, so counting it (or the
        // .tmp it is staged through, which exists only during the bake) would make every bake
        // invalidate itself.
        uint64_t SourceStamp(std::string_view sourcePath)
        {
            namespace fs = std::filesystem;

            uint64_t h = kFnvOffsetBasis;
            HashString(h, sourcePath);

            std::error_code ec;
            const fs::path  source = fs::path(sourcePath);
            const fs::path  root   = source.parent_path();
            if (root.empty() || !fs::is_directory(root, ec))
            {
                // No directory to walk (or it vanished): fall back to the file's own stamp.
                const auto size = fs::file_size(source, ec);
                if (!ec)
                    HashBytes(h, &size, sizeof(size));
                const auto time = fs::last_write_time(source, ec);
                if (!ec)
                {
                    const auto ticks = time.time_since_epoch().count();
                    HashBytes(h, &ticks, sizeof(ticks));
                }
                return h;
            }

            // Sorted so the hash does not depend on directory iteration order.
            std::vector<std::string> entries;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end;
                 it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const fs::path&   p        = it->path();
                const std::string filename = p.filename().string();
                if (filename.find(".vrfcache") != std::string::npos)
                    continue;

                std::error_code fileEc;
                const auto      size = fs::file_size(p, fileEc);
                const auto      time = fs::last_write_time(p, fileEc);
                if (fileEc)
                    continue;

                std::string entry = fs::relative(p, root, fileEc).generic_string();
                if (fileEc)
                    entry = p.filename().generic_string();
                entry += '|';
                // Both casts are load-bearing, not tidying targets: libc++'s
                // file_clock uses a __int128 duration rep, for which std::to_string is
                // ambiguous (it has no __int128 overload and several viable
                // conversions). File sizes and nanosecond timestamps fit in 64 bits
                // with room to spare, so narrowing here is safe.
                entry += std::to_string(static_cast<unsigned long long>(size));
                entry += '|';
                entry += std::to_string(static_cast<long long>(time.time_since_epoch().count()));
                entries.push_back(std::move(entry));
            }

            std::sort(entries.begin(), entries.end());
            for (const std::string& entry : entries)
                HashString(h, entry);
            return h;
        }

        struct Header
        {
            char     magic[8];
            uint32_t formatVersion;
            uint32_t loaderVersion;
            uint64_t sourceStamp;
            uint32_t flags;
            uint32_t reserved;
        };
        static_assert(sizeof(Header) == 32, "Header is the on-disk layout; keep it padding-free");

        // ---- writing ----------------------------------------------------------------
        // Streams straight to the file: a baked Sponza is hundreds of MB, and building the
        // payload in memory first would double peak RSS during the bake.
        class Writer
        {
        public:
            explicit Writer(std::ostream& out) : m_Out {out} {}

            void Raw(const void* data, size_t size)
            {
                m_Out.write(static_cast<const char*>(data), std::streamsize(size));
            }

            template<class T>
            void Pod(const T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                Raw(&value, sizeof(T));
            }

            void Str(const std::string& s)
            {
                Pod(static_cast<uint64_t>(s.size()));
                Raw(s.data(), s.size());
            }

            template<class T>
            void Array(const std::vector<T>& v)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                Pod(static_cast<uint64_t>(v.size()));
                Raw(v.data(), v.size() * sizeof(T));
            }

        private:
            std::ostream& m_Out;
        };

        // ---- reading ----------------------------------------------------------------
        // Every read is bounds-checked against the remaining file, so a truncated or
        // corrupted cache reports a miss instead of allocating a garbage-sized vector.
        class Reader
        {
        public:
            Reader(std::istream& in, uint64_t remaining) : m_In {in}, m_Remaining {remaining} {}

            [[nodiscard]] bool Ok() const noexcept { return m_Ok; }

            bool Raw(void* data, uint64_t size)
            {
                if (!m_Ok || size > m_Remaining)
                    return Fail();
                m_In.read(static_cast<char*>(data), std::streamsize(size));
                if (!m_In)
                    return Fail();
                m_Remaining -= size;
                return true;
            }

            template<class T>
            bool Pod(T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                return Raw(&value, sizeof(T));
            }

            bool Str(std::string& s)
            {
                uint64_t size = 0;
                if (!Pod(size) || size > m_Remaining)
                    return Fail();
                s.resize(static_cast<size_t>(size));
                return size == 0 || Raw(s.data(), size);
            }

            template<class T>
            bool Array(std::vector<T>& v)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                uint64_t count = 0;
                if (!Pod(count))
                    return false;
                // Check the byte count, not the element count, and check it before resizing.
                if (count > m_Remaining / sizeof(T))
                    return Fail();
                v.resize(static_cast<size_t>(count));
                return count == 0 || Raw(v.data(), count * sizeof(T));
            }

            // Element count for a non-trivially-copyable sequence, validated against a lower
            // bound on the per-element encoded size so a bogus count cannot force a huge resize.
            bool Count(uint64_t& count, uint64_t minBytesPerElement)
            {
                if (!Pod(count))
                    return false;
                if (minBytesPerElement != 0 && count > m_Remaining / minBytesPerElement)
                    return Fail();
                return true;
            }

        private:
            bool Fail()
            {
                m_Ok = false;
                return false;
            }

            std::istream& m_In;
            uint64_t      m_Remaining;
            bool          m_Ok = true;
        };

        // ---- material encoding -------------------------------------------------------
        // The variants are the only part of Mesh that is not a flat POD or a vector of PODs,
        // so they get an explicit tag byte rather than riding on std::variant::index(), whose
        // alternative order is a source-code detail rather than a format guarantee.
        enum class CoreTag : uint8_t
        {
            Unlit                 = 0,
            PbrMetallicRoughness  = 1,
            PbrSpecularGlossiness = 2,
            Phong                 = 3,
        };

        enum class ParamTag : uint8_t
        {
            Float      = 0,
            Int32      = 1,
            Bool       = 2,
            Vec2       = 3,
            Vec3       = 4,
            Vec4       = 5,
            TextureRef = 6,
            String     = 7,
        };

        void WriteParam(Writer& w, const MaterialParam& param)
        {
            std::visit(
                [&](auto&& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, float>)
                    {
                        w.Pod(ParamTag::Float);
                        w.Pod(value);
                    }
                    else if constexpr (std::is_same_v<T, int32_t>)
                    {
                        w.Pod(ParamTag::Int32);
                        w.Pod(value);
                    }
                    else if constexpr (std::is_same_v<T, bool>)
                    {
                        w.Pod(ParamTag::Bool);
                        w.Pod(static_cast<uint8_t>(value ? 1 : 0));
                    }
                    else if constexpr (std::is_same_v<T, Vec2>)
                    {
                        w.Pod(ParamTag::Vec2);
                        w.Pod(value);
                    }
                    else if constexpr (std::is_same_v<T, Vec3>)
                    {
                        w.Pod(ParamTag::Vec3);
                        w.Pod(value);
                    }
                    else if constexpr (std::is_same_v<T, Vec4>)
                    {
                        w.Pod(ParamTag::Vec4);
                        w.Pod(value);
                    }
                    else if constexpr (std::is_same_v<T, TextureRef>)
                    {
                        w.Pod(ParamTag::TextureRef);
                        w.Pod(static_cast<int32_t>(value.index));
                    }
                    else
                    {
                        w.Pod(ParamTag::String);
                        w.Str(value);
                    }
                },
                param);
        }

        bool ReadParam(Reader& r, MaterialParam& param)
        {
            ParamTag tag {};
            if (!r.Pod(tag))
                return false;
            switch (tag)
            {
                case ParamTag::Float: {
                    float v = 0.0f;
                    if (!r.Pod(v))
                        return false;
                    param = v;
                    return true;
                }
                case ParamTag::Int32: {
                    int32_t v = 0;
                    if (!r.Pod(v))
                        return false;
                    param = v;
                    return true;
                }
                case ParamTag::Bool: {
                    uint8_t v = 0;
                    if (!r.Pod(v))
                        return false;
                    param = v != 0;
                    return true;
                }
                case ParamTag::Vec2: {
                    Vec2 v {};
                    if (!r.Pod(v))
                        return false;
                    param = v;
                    return true;
                }
                case ParamTag::Vec3: {
                    Vec3 v {};
                    if (!r.Pod(v))
                        return false;
                    param = v;
                    return true;
                }
                case ParamTag::Vec4: {
                    Vec4 v {};
                    if (!r.Pod(v))
                        return false;
                    param = v;
                    return true;
                }
                case ParamTag::TextureRef: {
                    int32_t v = -1;
                    if (!r.Pod(v))
                        return false;
                    param = TextureRef {v};
                    return true;
                }
                case ParamTag::String: {
                    std::string v;
                    if (!r.Str(v))
                        return false;
                    param = std::move(v);
                    return true;
                }
            }
            return false;
        }

        void WriteMaterial(Writer& w, const Material& m)
        {
            w.Str(m.name);
            w.Pod(static_cast<uint8_t>(m.alphaMode));
            w.Pod(m.alphaCutoff);
            w.Pod(static_cast<uint8_t>(m.doubleSided ? 1 : 0));
            w.Pod(m.features);
            w.Str(m.customShader);

            std::visit(
                [&](auto&& payload) {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, UnlitMaterial>)
                        w.Pod(CoreTag::Unlit);
                    else if constexpr (std::is_same_v<T, PbrMetallicRoughnessMaterial>)
                        w.Pod(CoreTag::PbrMetallicRoughness);
                    else if constexpr (std::is_same_v<T, PbrSpecularGlossinessMaterial>)
                        w.Pod(CoreTag::PbrSpecularGlossiness);
                    else
                        w.Pod(CoreTag::Phong);
                    w.Pod(payload); // every payload is a flat POD of Vec*/float/TextureRef
                },
                m.core);

            w.Pod(static_cast<uint64_t>(m.extensions.size()));
            for (const auto& [name, extension] : m.extensions)
            {
                w.Str(name);
                w.Pod(static_cast<uint64_t>(extension.params.size()));
                for (const auto& [key, value] : extension.params)
                {
                    w.Str(key);
                    WriteParam(w, value);
                }
            }
        }

        bool ReadMaterial(Reader& r, Material& m)
        {
            uint8_t alphaMode = 0, doubleSided = 0;
            if (!r.Str(m.name) || !r.Pod(alphaMode) || !r.Pod(m.alphaCutoff) || !r.Pod(doubleSided) ||
                !r.Pod(m.features) || !r.Str(m.customShader))
                return false;
            m.alphaMode   = static_cast<AlphaMode>(alphaMode);
            m.doubleSided = doubleSided != 0;

            CoreTag tag {};
            if (!r.Pod(tag))
                return false;
            switch (tag)
            {
                case CoreTag::Unlit: {
                    UnlitMaterial payload {};
                    if (!r.Pod(payload))
                        return false;
                    m.core = payload;
                    break;
                }
                case CoreTag::PbrMetallicRoughness: {
                    PbrMetallicRoughnessMaterial payload {};
                    if (!r.Pod(payload))
                        return false;
                    m.core = payload;
                    break;
                }
                case CoreTag::PbrSpecularGlossiness: {
                    PbrSpecularGlossinessMaterial payload {};
                    if (!r.Pod(payload))
                        return false;
                    m.core = payload;
                    break;
                }
                case CoreTag::Phong: {
                    PhongMaterial payload {};
                    if (!r.Pod(payload))
                        return false;
                    m.core = payload;
                    break;
                }
                default:
                    return false;
            }

            uint64_t extensionCount = 0;
            if (!r.Count(extensionCount, sizeof(uint64_t) * 2))
                return false;
            for (uint64_t i = 0; i < extensionCount; ++i)
            {
                std::string name;
                uint64_t    paramCount = 0;
                if (!r.Str(name) || !r.Count(paramCount, sizeof(uint64_t) + 1))
                    return false;
                MaterialExtension extension;
                for (uint64_t p = 0; p < paramCount; ++p)
                {
                    std::string   key;
                    MaterialParam value;
                    if (!r.Str(key) || !ReadParam(r, value))
                        return false;
                    extension.params.emplace(std::move(key), std::move(value));
                }
                m.extensions.emplace(std::move(name), std::move(extension));
            }
            return true;
        }

        void WriteTexture(Writer& w, const Texture& t)
        {
            w.Str(t.name);
            w.Pod(t.width);
            w.Pod(t.height);
            w.Pod(t.depth);
            w.Pod(t.mipLevels);
            w.Pod(t.arrayLayers);
            w.Pod(static_cast<uint8_t>(t.isCubemap ? 1 : 0));
            w.Pod(static_cast<int32_t>(t.format));
            w.Pod(static_cast<uint8_t>(t.fileFormat));
            w.Pod(static_cast<uint8_t>(t.compressed ? 1 : 0));
            w.Array(t.subresources);
            w.Array(t.data);
        }

        bool ReadTexture(Reader& r, Texture& t)
        {
            uint8_t isCubemap = 0, fileFormat = 0, compressed = 0;
            int32_t format = 0;
            if (!r.Str(t.name) || !r.Pod(t.width) || !r.Pod(t.height) || !r.Pod(t.depth) || !r.Pod(t.mipLevels) ||
                !r.Pod(t.arrayLayers) || !r.Pod(isCubemap) || !r.Pod(format) || !r.Pod(fileFormat) ||
                !r.Pod(compressed) || !r.Array(t.subresources) || !r.Array(t.data))
                return false;
            t.isCubemap  = isCubemap != 0;
            t.format     = static_cast<VriFormat>(format);
            t.fileFormat = static_cast<TextureFileFormat>(fileFormat);
            t.compressed = compressed != 0;
            return true;
        }

#if defined(VRF_ENABLE_BAKE_BC7)
        // Block-compress one RGBA8 texture (with its mip chain) to BC7, in place.
        //
        // UASTC rather than ETC1S: UASTC is the visually-lossless mode, and it is designed to
        // transcode into BC7 with almost no further loss. The transcode runs here, offline, so
        // the cache stores hardware-decodable BC7 and startup stays a plain read - the whole
        // point of the cache. Storing UASTC instead would be ~half the disk but would put a
        // multi-second transcode back into every launch.
        bool CompressTextureBc7(Texture& texture)
        {
            if (texture.compressed || texture.data.empty())
                return false;
            if (texture.format != VriFormat_RGBA8_UNORM && texture.format != VriFormat_RGBA8_SRGB)
                return false;
            // BC7 works on 4x4 blocks; libktx pads, but a texture smaller than one block is not
            // worth a round trip.
            if (texture.width < 4 || texture.height < 4 || texture.depth != 1 || texture.isCubemap)
                return false;

            // Tells the encoder how to weight its error metric. The output is BC7_UNORM either
            // way - VRI exposes no BC7_SRGB - which is consistent with the glTF loader, whose
            // images are all RGBA8_UNORM with the sRGB decode done in the shader.
            const bool     srgb      = texture.format == VriFormat_RGBA8_SRGB;
            const uint32_t mipLevels = std::max(texture.mipLevels, 1u);

            ktxTextureCreateInfo info {};
            info.vkFormat        = srgb ? 43u /* VK_FORMAT_R8G8B8A8_SRGB */ : 37u /* ..._UNORM */;
            info.baseWidth       = texture.width;
            info.baseHeight      = texture.height;
            info.baseDepth       = 1;
            info.numDimensions   = 2;
            info.numLevels       = mipLevels;
            info.numLayers       = 1;
            info.numFaces        = 1;
            info.isArray         = KTX_FALSE;
            info.generateMipmaps = KTX_FALSE;

            ktxTexture2* ktx = nullptr;
            if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktx) != KTX_SUCCESS || ktx == nullptr)
                return false;

            // Feed each mip. Without an explicit subresource table the texture is a single
            // mip-0 image, which is the stb fast path.
            bool filled = true;
            for (uint32_t level = 0; level < mipLevels && filled; ++level)
            {
                const uint8_t* src   = nullptr;
                uint64_t       bytes = 0;
                if (texture.subresources.empty())
                {
                    if (level != 0)
                    {
                        filled = false;
                        break;
                    }
                    src   = texture.data.data();
                    bytes = texture.data.size();
                }
                else
                {
                    const auto it = std::find_if(
                        texture.subresources.begin(), texture.subresources.end(), [level](const TextureSubresource& s) {
                            return s.mipLevel == level && s.arrayLayer == 0;
                        });
                    if (it == texture.subresources.end() || it->offset + it->size > texture.data.size())
                    {
                        filled = false;
                        break;
                    }
                    src   = texture.data.data() + it->offset;
                    bytes = it->size;
                }
                if (ktxTexture_SetImageFromMemory(ktxTexture(ktx), level, 0, 0, src, size_t(bytes)) != KTX_SUCCESS)
                    filled = false;
            }
            if (!filled)
            {
                ktxTexture_Destroy(ktxTexture(ktx));
                return false;
            }

            ktxBasisParams params {};
            params.structSize = sizeof(params);
            params.uastc      = KTX_TRUE;
            // One libktx thread: the caller already runs one texture per hardware thread, so
            // nesting a second pool would just oversubscribe.
            params.threadCount = 1;

            if (ktxTexture2_CompressBasisEx(ktx, &params) != KTX_SUCCESS ||
                ktxTexture2_TranscodeBasis(ktx, KTX_TTF_BC7_RGBA, 0) != KTX_SUCCESS)
            {
                ktxTexture_Destroy(ktxTexture(ktx));
                return false;
            }

            // Repack into our own flat blob + subresource table, so the reader stays libktx-free.
            std::vector<uint8_t>            data;
            std::vector<TextureSubresource> subresources;
            data.reserve(ktxTexture_GetDataSize(ktxTexture(ktx)));
            subresources.reserve(mipLevels);
            for (uint32_t level = 0; level < mipLevels; ++level)
            {
                ktx_size_t offset = 0;
                if (ktxTexture_GetImageOffset(ktxTexture(ktx), level, 0, 0, &offset) != KTX_SUCCESS)
                {
                    ktxTexture_Destroy(ktxTexture(ktx));
                    return false;
                }
                const ktx_size_t size = ktxTexture_GetImageSize(ktxTexture(ktx), level);
                const uint8_t*   src  = ktxTexture_GetData(ktxTexture(ktx)) + offset;

                TextureSubresource sub {};
                sub.mipLevel   = level;
                sub.arrayLayer = 0;
                sub.offset     = data.size();
                sub.size       = size;
                sub.width      = std::max(texture.width >> level, 1u);
                sub.height     = std::max(texture.height >> level, 1u);
                subresources.push_back(sub);
                data.insert(data.end(), src, src + size);
            }
            ktxTexture_Destroy(ktxTexture(ktx));

            texture.data         = std::move(data);
            texture.subresources = std::move(subresources);
            texture.mipLevels    = mipLevels;
            texture.format       = VriFormat_BC7_UNORM;
            texture.compressed   = true;
            return true;
        }

        // Physical memory currently available, or 0 when the platform cannot say. Only used to
        // BOUND the encoder pool below, never to decide whether to compress at all.
        uint64_t AvailablePhysicalBytes()
        {
#if defined(_WIN32)
            MEMORYSTATUSEX status {};
            status.dwLength = sizeof(status);
            return GlobalMemoryStatusEx(&status) ? uint64_t {status.ullAvailPhys} : 0ull;
#elif defined(__linux__)
            const long pages    = sysconf(_SC_AVPHYS_PAGES);
            const long pageSize = sysconf(_SC_PAGE_SIZE);
            return (pages > 0 && pageSize > 0) ? uint64_t(pages) * uint64_t(pageSize) : 0ull;
#elif defined(__APPLE__)
            vm_statistics64_data_t stats {};
            mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
            if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) !=
                KERN_SUCCESS)
                return 0ull;
            vm_size_t pageSize = 0;
            if (host_page_size(mach_host_self(), &pageSize) != KERN_SUCCESS)
                return 0ull;
            return uint64_t(stats.free_count + stats.inactive_count) * uint64_t(pageSize);
#else
            return 0ull;
#endif
        }

        // One texture per hardware thread, BOUNDED BY FREE MEMORY. The bake's peak is not the
        // encode itself but what surrounds it: every decoded RGBA8 texture in the model is already
        // resident (a real scene runs to several GB), and each worker allocates a full ktx copy of
        // its texture plus the UASTC encoder's scratch on top of that. Sizing the pool by core
        // count alone made the transient scale with the machine's parallelism while the resident
        // set stayed fixed, so a high-core / modest-RAM machine (20 threads, 16 GB) died of
        // std::bad_alloc mid-bake - and an allocation failure inside a worker thread is an
        // uncaught exception, i.e. terminate(), i.e. the whole bake lost with only a .tmp behind.
        // Encoding is embarrassingly parallel, so a smaller pool costs wall clock and nothing else.
        void CompressTexturesBc7(Mesh& mesh)
        {
            if (mesh.textures.empty())
                return;

            const uint64_t      before = std::accumulate(mesh.textures.begin(),
                                                    mesh.textures.end(),
                                                    uint64_t {0},
                                                    [](uint64_t sum, const Texture& t) { return sum + t.SizeBytes(); });
            std::atomic<size_t> next {0};
            std::atomic<size_t> converted {0};

            unsigned workers = std::max(
                1u,
                std::min<unsigned>(std::thread::hardware_concurrency(), static_cast<unsigned>(mesh.textures.size())));
            if (const uint64_t available = AvailablePhysicalBytes(); available != 0)
            {
                // Budget per worker: the largest texture, counted a few times over for the ktx
                // storage copy, the transcode target and the encoder's own scratch. Deliberately
                // pessimistic - overestimating costs wall clock, underestimating loses the bake.
                const uint64_t largest = std::accumulate(
                    mesh.textures.begin(), mesh.textures.end(), uint64_t {0}, [](uint64_t m, const Texture& t) {
                        return std::max(m, t.SizeBytes());
                    });
                const uint64_t perWorker = std::max<uint64_t>(largest * 4ull, 64ull * 1024 * 1024);
                // Spend half of what is free: the rest absorbs the writer's own buffers and
                // whatever else the machine is doing while a multi-minute bake runs.
                const auto affordable = static_cast<unsigned>(std::max<uint64_t>(available / 2ull / perWorker, 1ull));
                if (affordable < workers)
                {
                    LogInfo("asset cache: BC7 pool {} -> {} threads ({} MB free, ~{} MB per worker)",
                            workers,
                            affordable,
                            available / (1024 * 1024),
                            perWorker / (1024 * 1024));
                    workers = affordable;
                }
            }
            std::vector<std::thread> pool;
            pool.reserve(workers);
            for (unsigned w = 0; w < workers; ++w)
            {
                pool.emplace_back([&] {
                    for (size_t i = next++; i < mesh.textures.size(); i = next++)
                    {
                        // A worker that still runs out of memory must not take the process with
                        // it: the texture simply stays uncompressed, which the format already
                        // supports (Texture::compressed is per texture), and the bake completes.
                        try
                        {
                            if (CompressTextureBc7(mesh.textures[i]))
                                ++converted;
                        }
                        catch (const std::bad_alloc&)
                        {
                            LogWarning("asset cache: out of memory compressing texture {}, leaving it uncompressed",
                                       i);
                        }
                    }
                });
            }
            for (std::thread& t : pool)
                t.join();

            const uint64_t after = std::accumulate(mesh.textures.begin(),
                                                   mesh.textures.end(),
                                                   uint64_t {0},
                                                   [](uint64_t sum, const Texture& t) { return sum + t.SizeBytes(); });
            LogInfo("asset cache: BC7 compressed {}/{} textures, {} MB -> {} MB",
                    converted.load(),
                    mesh.textures.size(),
                    before / (1024 * 1024),
                    after / (1024 * 1024));
        }
#else
        void CompressTexturesBc7(Mesh&) {}
#endif
    } // namespace

    Expected<void> WriteBakedMesh(const std::string_view cachePath, const std::string_view sourcePath, const Mesh& mesh)
    {
        namespace fs = std::filesystem;

        // Write to a temporary and rename, so a crash or a concurrent reader never sees a
        // half-written cache that would then have to be detected as corrupt.
        const fs::path finalPath = fs::path(cachePath);
        const fs::path tempPath  = fs::path(std::string(cachePath) + ".tmp");

        // Stamp the sources before creating anything, so the bake cannot observe its own
        // intermediate files even if the exclusion above is ever loosened.
        const uint64_t sourceStamp = SourceStamp(sourcePath);

        {
            std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
            if (!out)
                return MakeError("WriteBakedMesh: cannot open " + tempPath.string());

            Header header {};
            std::memcpy(header.magic, kMagic, sizeof(kMagic));
            header.formatVersion = kFormatVersion;
            header.loaderVersion = kLoaderVersion;
            header.sourceStamp   = sourceStamp;
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));

            Writer w {out};
            w.Str(mesh.name);
            w.Pod(static_cast<uint32_t>(mesh.attributes));
            w.Pod(mesh.boundsMin);
            w.Pod(mesh.boundsMax);

            w.Array(mesh.positions);
            w.Array(mesh.normals);
            w.Array(mesh.tangents);
            w.Array(mesh.colors);
            w.Array(mesh.texCoords0);
            w.Array(mesh.texCoords1);
            w.Array(mesh.indices);

            w.Pod(static_cast<uint64_t>(mesh.subMeshes.size()));
            for (const SubMesh& sub : mesh.subMeshes)
            {
                w.Str(sub.name);
                w.Pod(sub.indexOffset);
                w.Pod(sub.indexCount);
                w.Pod(sub.vertexOffset);
                w.Pod(sub.vertexCount);
                w.Pod(static_cast<int32_t>(sub.materialIndex));
            }

            w.Pod(static_cast<uint64_t>(mesh.materials.size()));
            for (const Material& material : mesh.materials)
                WriteMaterial(w, material);

            w.Pod(static_cast<uint64_t>(mesh.textures.size()));
            for (const Texture& texture : mesh.textures)
                WriteTexture(w, texture);

            out.flush();
            if (!out)
                return MakeError("WriteBakedMesh: write failed for " + tempPath.string());
        }

        std::error_code ec;
        fs::rename(tempPath, finalPath, ec);
        if (ec)
        {
            fs::remove(tempPath, ec);
            return MakeError("WriteBakedMesh: rename failed for " + finalPath.string());
        }
        return {};
    }

    Expected<void> ReadBakedMesh(const std::string_view cachePath, const std::string_view sourcePath, Mesh& out)
    {
        namespace fs = std::filesystem;

        std::error_code ec;
        const uint64_t  fileSize = fs::file_size(fs::path(cachePath), ec);
        if (ec || fileSize < sizeof(Header))
            return MakeError("ReadBakedMesh: no usable cache at " + std::string(cachePath));

        std::ifstream in(fs::path(cachePath), std::ios::binary);
        if (!in)
            return MakeError("ReadBakedMesh: cannot open " + std::string(cachePath));

        Header header {};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0)
            return MakeError("ReadBakedMesh: bad magic in " + std::string(cachePath));
        if (header.formatVersion != kFormatVersion || header.loaderVersion != kLoaderVersion)
            return MakeError("ReadBakedMesh: version mismatch in " + std::string(cachePath));
        if (header.sourceStamp != SourceStamp(sourcePath))
            return MakeError("ReadBakedMesh: source changed since " + std::string(cachePath) + " was baked");

        Mesh   mesh;
        Reader r {in, fileSize - sizeof(Header)};

        uint32_t attributes = 0;
        if (!r.Str(mesh.name) || !r.Pod(attributes) || !r.Pod(mesh.boundsMin) || !r.Pod(mesh.boundsMax))
            return MakeError("ReadBakedMesh: truncated header section");
        mesh.attributes = static_cast<VertexAttribute>(attributes);

        if (!r.Array(mesh.positions) || !r.Array(mesh.normals) || !r.Array(mesh.tangents) || !r.Array(mesh.colors) ||
            !r.Array(mesh.texCoords0) || !r.Array(mesh.texCoords1) || !r.Array(mesh.indices))
            return MakeError("ReadBakedMesh: truncated vertex streams");

        uint64_t subMeshCount = 0;
        if (!r.Count(subMeshCount, sizeof(uint64_t) + sizeof(uint32_t) * 4 + sizeof(int32_t)))
            return MakeError("ReadBakedMesh: bad submesh count");
        mesh.subMeshes.resize(static_cast<size_t>(subMeshCount));
        for (SubMesh& sub : mesh.subMeshes)
        {
            int32_t materialIndex = -1;
            if (!r.Str(sub.name) || !r.Pod(sub.indexOffset) || !r.Pod(sub.indexCount) || !r.Pod(sub.vertexOffset) ||
                !r.Pod(sub.vertexCount) || !r.Pod(materialIndex))
                return MakeError("ReadBakedMesh: truncated submesh table");
            sub.materialIndex = materialIndex;
        }

        uint64_t materialCount = 0;
        if (!r.Count(materialCount, sizeof(uint64_t)))
            return MakeError("ReadBakedMesh: bad material count");
        mesh.materials.resize(static_cast<size_t>(materialCount));
        for (Material& material : mesh.materials)
            if (!ReadMaterial(r, material))
                return MakeError("ReadBakedMesh: truncated material table");

        uint64_t textureCount = 0;
        if (!r.Count(textureCount, sizeof(uint64_t)))
            return MakeError("ReadBakedMesh: bad texture count");
        mesh.textures.resize(static_cast<size_t>(textureCount));
        for (Texture& texture : mesh.textures)
            if (!ReadTexture(r, texture))
                return MakeError("ReadBakedMesh: truncated texture table");

        if (!r.Ok())
            return MakeError("ReadBakedMesh: truncated payload in " + std::string(cachePath));

        out = std::move(mesh);
        return {};
    }

    Expected<void> LoadModelCached(const std::string_view   path,
                                   Mesh&                    out,
                                   const GltfImportOptions& options,
                                   const AssetCacheOptions& cache)
    {
        namespace fs = std::filesystem;

        const auto loadSource = [&]() -> Expected<void> {
            const std::string ext = fs::path(path).extension().string();
            if (ext == ".obj" || ext == ".OBJ")
                return LoadObj(path, out);
            return LoadGltf(path, out, options);
        };

        if (!cache.enabled)
            return loadSource();

        const std::string cachePath = cache.cachePath.empty() ? std::string(path) + ".vrfcache" : cache.cachePath;

        if (auto hit = ReadBakedMesh(cachePath, path, out); hit)
        {
            LogInfo("asset cache: hit {} ({} vertices, {} submeshes, {} textures)",
                    cachePath,
                    out.VertexCount(),
                    out.subMeshes.size(),
                    out.textures.size());
            return {};
        }

        if (auto loaded = loadSource(); !loaded)
            return loaded;

        if (!cache.write)
            return {};

        // Bake the tangents in too. Without this every startup still pays GenerateTangents
        // inside UploadMesh, which is one of the costs the cache exists to remove.
        if (out.tangents.size() != out.positions.size() && out.normals.size() == out.positions.size() &&
            out.texCoords0.size() == out.positions.size())
        {
            out.tangents = GenerateTangents(out);
            out.attributes |= VertexAttribute::Tangent;
        }

        CompressTexturesBc7(out);

        // A bake failure must not fail the load - the mesh in `out` is already good.
        if (auto baked = WriteBakedMesh(cachePath, path, out); !baked)
            LogWarning("asset cache: could not bake {}: {}", cachePath, baked.error().message);
        else
            LogInfo("asset cache: baked {}", cachePath);

        return {};
    }
} // namespace vrf
