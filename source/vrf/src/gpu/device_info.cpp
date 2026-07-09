#include "vrf/gpu/device_info.hpp"

#include "vrf/gpu/render_device.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace vrf
{
    namespace
    {
        const char* AdapterTypeName(VriAdapterType type)
        {
            switch (type)
            {
                case VriAdapterType_Discrete:
                    return "discrete";
                case VriAdapterType_Integrated:
                    return "integrated";
                case VriAdapterType_Software:
                    return "software";
                default:
                    return "unknown";
            }
        }

        // Best-effort PCI vendor-ID -> name (matches the common desktop/mobile vendors).
        const char* VendorName(uint32_t vendorId)
        {
            switch (vendorId)
            {
                case 0x10DE:
                    return "NVIDIA";
                case 0x1002:
                case 0x1022:
                    return "AMD";
                case 0x8086:
                    return "Intel";
                case 0x13B5:
                    return "ARM";
                case 0x5143:
                    return "Qualcomm";
                case 0x106B:
                    return "Apple";
                case 0x1010:
                    return "ImgTec";
                case 0x0000:
                    return "n/a";
                default:
                    return "unknown";
            }
        }

        std::string Mib(uint64_t bytes) { return std::to_string(bytes / (1024ull * 1024ull)) + " MiB"; }

        // Emit "labelPad: value" then a newline (two-space indent, aligned values).
        void Row(std::ostringstream& out, const char* label, const std::string& value)
        {
            out << "  " << label;
            for (std::size_t i = std::string(label).size(); i < 14; ++i)
                out << ' ';
            out << ": " << value << '\n';
        }

        // Join names with ", ", wrapping to keep lines readable under a fixed indent.
        std::string WrapJoin(const std::vector<const char*>& names, std::size_t indent, std::size_t width)
        {
            std::string       result;
            std::size_t       column = indent;
            const std::string pad(indent, ' ');
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                const std::string item = std::string(names[i]) + (i + 1 < names.size() ? ", " : "");
                if (column + item.size() > width && column > indent)
                {
                    result += '\n' + pad;
                    column = indent;
                }
                result += item;
                column += item.size();
            }
            return result.empty() ? "(none)" : result;
        }
    } // namespace

    std::string DescribeDevice(const RenderDevice& device)
    {
        std::ostringstream   out;
        const VriDeviceDesc* d = device.Desc();
        out << "Render device\n";
        if (d == nullptr)
        {
            Row(out, "API", device.ApiName());
            return out.str();
        }

        std::string api = device.ApiName();
        if (d->apiVersionMajor != 0 || d->apiVersionMinor != 0)
            api += " " + std::to_string(d->apiVersionMajor) + "." + std::to_string(d->apiVersionMinor);
        Row(out, "API", api);
        Row(out, "Adapter", d->adapter.name[0] != '\0' ? d->adapter.name : "(unnamed)");
        Row(out,
            "Type/vendor",
            std::string(AdapterTypeName(d->adapter.type)) + " / " + VendorName(d->adapter.vendorId));

        std::string mem = Mib(d->adapter.videoMemorySize);
        if (d->adapter.sharedMemorySize != 0)
            mem += " (shared " + Mib(d->adapter.sharedMemorySize) + ")";
        Row(out, "Video memory", mem);

        Row(out,
            "Queues",
            "graphics x" + std::to_string(d->queueCount[VriQueueType_Graphics]) + ", compute x" +
                std::to_string(d->queueCount[VriQueueType_Compute]) + ", transfer x" +
                std::to_string(d->queueCount[VriQueueType_Transfer]));

        if (d->subgroupSize != 0)
            Row(out, "Subgroup", std::to_string(d->subgroupSize) + " lanes");

        Row(out,
            "Limits",
            "tex2D " + std::to_string(d->texture2DMaxDim) + ", tex3D " + std::to_string(d->texture3DMaxDim) +
                ", array " + std::to_string(d->textureArrayLayerMaxNum) + ", buffer " + Mib(d->bufferMaxSize) +
                ", color attach " + std::to_string(d->attachmentColorMaxNum) + ", viewports " +
                std::to_string(d->viewportMaxNum));

        std::vector<const char*> core;
        if (d->hasComputeShader)
            core.push_back("compute");
        if (d->hasGeometryShader)
            core.push_back("geometry");
        if (d->hasTessellation)
            core.push_back("tessellation");
        Row(out, "Shaders", WrapJoin(core, 18, 96));

        // Optional feature matrix (only the supported ones are listed).
        std::vector<const char*> feats;
        auto                     add = [&](VriBool has, const char* name) {
            if (has)
                feats.push_back(name);
        };
        add(d->hasRayTracing, "ray-tracing");
        add(d->hasRayQuery, "ray-query");
        add(d->hasMeshShader, "mesh-shader");
        add(d->hasBindless, "bindless");
        add(d->hasVariableShadingRate, "variable-rate-shading");
        add(d->hasOpacityMicromap, "opacity-micromap");
        add(d->hasConservativeRaster, "conservative-raster");
        add(d->hasFragmentShaderBarycentric, "fragment-barycentric");
        add(d->hasCustomBorderColor, "custom-border-color");
        add(d->hasShaderWaveOps, "wave-ops");
        add(d->hasExternalMemory, "external-memory");
        add(d->hasTimestampQueries, "timestamp-queries");
        add(d->hasPipelineStatistics, "pipeline-statistics");
        add(d->hasCalibratedTimestamps, "calibrated-timestamps");
        add(d->hasDrawIndirectCount, "draw-indirect-count");
        add(d->hasClearStorageBuffer, "clear-storage-buffer");
        add(d->hasClearStorageTexture, "clear-storage-texture");
        if (d->hasMultiview)
            feats.push_back("multiview");
        Row(out, "Features", WrapJoin(feats, 18, 96));
        if (d->hasMultiview && d->maxViewCount != 0)
            Row(out, "Max views", std::to_string(d->maxViewCount));

        return out.str();
    }
} // namespace vrf
