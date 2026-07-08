#include "vrf/asset/loaders/gaussian_splat_loader.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// GaussForge (+ spz) is the single Gaussian backend; its headers are confined to this .cpp.
#include <gf/core/gauss_ir.h>
#include <gf/io/reader.h>
#include <gf/io/registry.h>

namespace vrf
{
    namespace
    {
        std::vector<uint8_t> ReadBinaryFile(const std::string& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            if (size <= 0)
                return {};
            file.seekg(0);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            file.read(reinterpret_cast<char*>(bytes.data()), size);
            return bytes;
        }

        std::string LowerExtension(std::string_view path)
        {
            const size_t dot = path.find_last_of('.');
            if (dot == std::string_view::npos)
                return {};
            std::string ext(path.substr(dot + 1));
            for (char& ch : ext)
                ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
            return ext;
        }

        // GaussForge registry key: *.compressed.ply -> "compressed.ply", else the lowercase
        // extension (ply / spz / splat / ksplat).
        std::string GaussForgeFormatKey(std::string_view path)
        {
            std::string lower(path);
            for (char& ch : lower)
                ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
            const std::string_view suffix = ".compressed.ply";
            if (lower.size() >= suffix.size() &&
                lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0)
                return "compressed.ply";
            return LowerExtension(path);
        }

        // Decode `path` with the GaussForge reader registered for `key` and map its structure-of-
        // arrays IR onto the framework's 64-byte GaussianSplatPoint. Values are copied raw (opacity
        // = logit, scale = log-space, SH DC as stored); only the quaternion is reordered from the
        // IR's [w,x,y,z] to the point's xyzw. No coordinate-convention change is applied - the
        // app/renderer decides. Higher-order SH is carried verbatim in `sh`.
        Expected<void> LoadGaussianSplatWithKey(std::string_view path, const std::string& key, GaussianSplat& out)
        {
            const std::vector<uint8_t> bytes = ReadBinaryFile(std::string(path));
            if (bytes.empty())
                return MakeError(std::string("LoadGaussianSplat: cannot read file: ") + std::string(path));

            static const gf::IORegistry registry; // registers all built-in readers once
            gf::IGaussReader*           reader = registry.ReaderForExt(key);
            if (reader == nullptr)
                return MakeError("LoadGaussianSplat: unsupported gaussian format: " + key);

            gf::ReadOptions                   readOptions {};
            gf::Expected<gf::GaussianCloudIR> ir = reader->Read(bytes.data(), bytes.size(), readOptions);
            if (!ir)
                return MakeError("LoadGaussianSplat: GaussForge read failed: " + ir.error().message);

            const gf::GaussianCloudIR& cloud = ir.value();
            const int32_t              n     = cloud.numPoints;
            if (n < 0 || cloud.positions.size() < static_cast<size_t>(n) * 3)
                return MakeError("LoadGaussianSplat: malformed gaussian cloud");

            const bool hasScales    = cloud.scales.size() >= static_cast<size_t>(n) * 3;
            const bool hasRotations = cloud.rotations.size() >= static_cast<size_t>(n) * 4;
            const bool hasAlphas    = cloud.alphas.size() >= static_cast<size_t>(n);
            const bool hasColors    = cloud.colors.size() >= static_cast<size_t>(n) * 3;

            out             = GaussianSplat {};
            out.name        = std::string(path);
            out.numPoints   = n;
            out.shDegree    = cloud.meta.shDegree;
            out.antialiased = cloud.meta.antialiased;
            out.sh          = cloud.sh;
            out.splats.resize(static_cast<size_t>(n));
            for (int32_t i = 0; i < n; ++i)
            {
                const size_t       i3 = static_cast<size_t>(i) * 3;
                const size_t       i4 = static_cast<size_t>(i) * 4;
                GaussianSplatPoint p {};
                p.position = Vec3(cloud.positions[i3 + 0], cloud.positions[i3 + 1], cloud.positions[i3 + 2]);
                p.opacity  = hasAlphas ? cloud.alphas[static_cast<size_t>(i)] : 0.0f;
                p.scale =
                    hasScales ? Vec3(cloud.scales[i3 + 0], cloud.scales[i3 + 1], cloud.scales[i3 + 2]) : Vec3(0.0f);
                // IR quaternion is [w,x,y,z]; GaussianSplatPoint stores xyzw.
                p.rotation = hasRotations ? Vec4(cloud.rotations[i4 + 1],
                                                 cloud.rotations[i4 + 2],
                                                 cloud.rotations[i4 + 3],
                                                 cloud.rotations[i4 + 0]) :
                                            Vec4(0.0f, 0.0f, 0.0f, 1.0f);
                p.shDC =
                    hasColors ? Vec3(cloud.colors[i3 + 0], cloud.colors[i3 + 1], cloud.colors[i3 + 2]) : Vec3(0.0f);
                out.splats[static_cast<size_t>(i)] = p;
            }
            return {};
        }
    } // namespace

    Expected<void> LoadGaussianSplatPly(std::string_view path, GaussianSplat& out)
    {
        return LoadGaussianSplatWithKey(path, "ply", out);
    }

    Expected<void> LoadGaussianSplatSplat(std::string_view path, GaussianSplat& out)
    {
        return LoadGaussianSplatWithKey(path, "splat", out);
    }

    Expected<void> LoadGaussianSplat(std::string_view path, GaussianSplat& out)
    {
        return LoadGaussianSplatWithKey(path, GaussForgeFormatKey(path), out);
    }
} // namespace vrf
