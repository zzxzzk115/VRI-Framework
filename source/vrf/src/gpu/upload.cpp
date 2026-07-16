#include "vrf/gpu/upload.hpp"

#include <cstring>
#include <functional>
#include <vector>

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    // Record `record` on a throwaway command buffer, submit, and wait. Public (declared in
    // upload.hpp) so callers can build acceleration structures / do one-off GPU work at load time.
    void ImmediateSubmit(RenderDevice& device, const std::function<void(VriCommandBuffer*)>& record)
    {
        const VriCoreInterface& c = device.Core();

        VriCommandAllocator* alloc = nullptr;
        c.CreateCommandAllocator(device.Handle(), VriQueueType_Graphics, &alloc);
        VriCommandBuffer* cmd = nullptr;
        c.CreateCommandBuffer(alloc, &cmd);
        VriFence* fence = nullptr;
        c.CreateFence(device.Handle(), 0, &fence);

        c.ResetCommandAllocator(alloc);
        c.BeginCommandBuffer(cmd);
        record(cmd);
        c.EndCommandBuffer(cmd);

        VriFenceSubmitDesc signal {};
        signal.fence  = fence;
        signal.value  = 1;
        signal.stages = VriPipelineStage_AllCommands;
        VriQueueSubmitDesc submit {};
        submit.commandBuffers   = &cmd;
        submit.commandBufferNum = 1;
        submit.signalFences     = &signal;
        submit.signalFenceNum   = 1;
        c.QueueSubmit(device.GraphicsQueue(), &submit);
        c.Wait(fence, 1);

        c.DestroyFence(fence);
        c.DestroyCommandAllocator(alloc);
    }

    namespace
    {
        Expected<VriBuffer*> CreateDeviceBuffer(RenderDevice& device, uint64_t size, VriBufferUsageFlags usage)
        {
            VriBufferDesc desc {};
            desc.size           = size;
            desc.usage          = usage | VriBufferUsage_TransferDst;
            desc.memoryLocation = VriMemoryLocation_Device;
            VriBuffer* buffer   = nullptr;
            if (device.Core().CreateBuffer(device.Handle(), &desc, &buffer) != VriResult_Success)
                return MakeError("upload: CreateBuffer (device-local) failed");
            return buffer;
        }

        Expected<VriBuffer*> CreateStagingBuffer(RenderDevice& device, const void* data, uint64_t size)
        {
            const VriCoreInterface& c = device.Core();
            VriBufferDesc           desc {};
            desc.size           = size;
            desc.usage          = VriBufferUsage_TransferSrc;
            desc.memoryLocation = VriMemoryLocation_HostUpload;
            VriBuffer* buffer   = nullptr;
            if (c.CreateBuffer(device.Handle(), &desc, &buffer) != VriResult_Success)
                return MakeError("upload: CreateBuffer (staging) failed");
            std::memcpy(c.MapBuffer(buffer, 0, size), data, size);
            c.UnmapBuffer(buffer);
            return buffer;
        }

        // Texel-block geometry for buffer<->image copies: block dimensions in texels and bytes
        // per block. Uncompressed formats are 1x1 blocks of `blockBytes` bytes per texel.
        struct FormatBlockInfo
        {
            uint32_t blockWidth  = 1;
            uint32_t blockHeight = 1;
            uint32_t blockBytes  = 4;
            bool     compressed  = false;
        };

        FormatBlockInfo GetFormatBlockInfo(VriFormat format)
        {
            switch (format)
            {
                case VriFormat_BC1_UNORM:
                case VriFormat_BC4_UNORM:
                    return {4, 4, 8, true};
                case VriFormat_BC2_UNORM:
                case VriFormat_BC3_UNORM:
                case VriFormat_BC5_UNORM:
                case VriFormat_BC6H_UFLOAT:
                case VriFormat_BC7_UNORM:
                    return {4, 4, 16, true};
                case VriFormat_R8_UNORM:
                case VriFormat_R8_SNORM:
                case VriFormat_R8_UINT:
                case VriFormat_R8_SINT:
                    return {1, 1, 1, false};
                case VriFormat_RG8_UNORM:
                case VriFormat_RG8_SNORM:
                case VriFormat_R16_UNORM:
                case VriFormat_R16_SFLOAT:
                case VriFormat_R16_UINT:
                case VriFormat_R16_SINT:
                    return {1, 1, 2, false};
                case VriFormat_RGBA8_UNORM:
                case VriFormat_RGBA8_SRGB:
                case VriFormat_BGRA8_UNORM:
                case VriFormat_BGRA8_SRGB:
                case VriFormat_RG16_UNORM:
                case VriFormat_RG16_SFLOAT:
                case VriFormat_R32_SFLOAT:
                case VriFormat_R32_UINT:
                case VriFormat_R32_SINT:
                case VriFormat_RGB10A2_UNORM:
                case VriFormat_RG11B10_UFLOAT:
                    return {1, 1, 4, false};
                case VriFormat_RGBA16_UNORM:
                case VriFormat_RGBA16_SFLOAT:
                case VriFormat_RG32_SFLOAT:
                    return {1, 1, 8, false};
                case VriFormat_RGBA32_SFLOAT:
                    return {1, 1, 16, false};
                default:
                    return {1, 1, 4, false};
            }
        }

        uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

        // Per-vertex tangents (xyz + handedness w) from positions/uvs/normals (Lengyel's method),
        // used when the mesh doesn't provide TANGENT. Falls back to a stable default without uvs.
        std::vector<glm::vec4> GenerateTangents(const Mesh& mesh)
        {
            const size_t           vcount  = mesh.positions.size();
            const bool             hasUV   = mesh.texCoords0.size() == vcount;
            const bool             hasNorm = mesh.normals.size() == vcount;
            std::vector<glm::vec3> tan1(vcount, glm::vec3(0.0f));
            std::vector<glm::vec3> tan2(vcount, glm::vec3(0.0f));

            const auto processTri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
                if (i0 >= vcount || i1 >= vcount || i2 >= vcount)
                    return;
                const glm::vec3 e1  = mesh.positions[i1] - mesh.positions[i0];
                const glm::vec3 e2  = mesh.positions[i2] - mesh.positions[i0];
                const glm::vec2 uv0 = hasUV ? mesh.texCoords0[i0] : glm::vec2(0.0f);
                const glm::vec2 uv1 = hasUV ? mesh.texCoords0[i1] : glm::vec2(0.0f);
                const glm::vec2 uv2 = hasUV ? mesh.texCoords0[i2] : glm::vec2(0.0f);
                const glm::vec2 d1  = uv1 - uv0;
                const glm::vec2 d2  = uv2 - uv0;
                const float     det = d1.x * d2.y - d2.x * d1.y;
                const float     r   = glm::abs(det) > 1e-12f ? 1.0f / det : 0.0f;
                const glm::vec3 s   = (e1 * d2.y - e2 * d1.y) * r;
                const glm::vec3 t   = (e2 * d1.x - e1 * d2.x) * r;
                tan1[i0] += s;
                tan1[i1] += s;
                tan1[i2] += s;
                tan2[i0] += t;
                tan2[i1] += t;
                tan2[i2] += t;
            };

            if (!mesh.indices.empty())
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                    processTri(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]);
            else
                for (size_t i = 0; i + 2 < vcount; i += 3)
                    processTri(static_cast<uint32_t>(i), static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i + 2));

            std::vector<glm::vec4> out(vcount);
            for (size_t i = 0; i < vcount; ++i)
            {
                const glm::vec3 n       = hasNorm ? mesh.normals[i] : glm::vec3(0.0f, 0.0f, 1.0f);
                const glm::vec3 t       = tan1[i];
                glm::vec3       tangent = t - n * glm::dot(n, t); // Gram-Schmidt orthogonalize
                const float     len     = glm::length(tangent);
                tangent                 = len > 1e-8f ? tangent / len : glm::vec3(1.0f, 0.0f, 0.0f);
                const float w           = glm::dot(glm::cross(n, t), tan2[i]) < 0.0f ? -1.0f : 1.0f; // handedness
                out[i]                  = glm::vec4(tangent, w);
            }
            return out;
        }
    } // namespace

    void GpuMesh::Destroy(RenderDevice& device)
    {
        const VriCoreInterface& c = device.Core();
        if (indexBuffer)
            c.DestroyBuffer(indexBuffer);
        if (vertexBuffer)
            c.DestroyBuffer(vertexBuffer);
        vertexBuffer = nullptr;
        indexBuffer  = nullptr;
    }

    void GpuTexture::Destroy(RenderDevice& device)
    {
        const VriCoreInterface& c = device.Core();
        if (view)
            c.DestroyDescriptor(view);
        if (texture)
            c.DestroyTexture(texture);
        view    = nullptr;
        texture = nullptr;
    }

    Expected<GpuMesh> UploadMesh(RenderDevice& device, const Mesh& mesh, bool rtAccelInput)
    {
        if (mesh.positions.empty())
            return MakeError("UploadMesh: mesh has no positions");

        const VriCoreInterface& c            = device.Core();
        const size_t            vcount       = mesh.positions.size();
        const bool              hasNormals   = mesh.normals.size() == vcount;
        const bool              hasTexCoords = mesh.texCoords0.size() == vcount;
        const bool              hasColors    = mesh.colors.size() == vcount;
        const bool              hasTangents  = mesh.tangents.size() == vcount;
        // Carry a tangent whenever the mesh provides one, or can support normal mapping
        // (normals + uvs) - in which case we generate it. Untextured meshes carry none.
        const bool wantTangent = hasTangents || (hasNormals && hasTexCoords);

        // The compact interleaved layout is driven by which attributes the mesh actually has.
        VertexAttribute mask = VertexAttribute::Position;
        if (hasNormals)
            mask |= VertexAttribute::Normal;
        if (wantTangent)
            mask |= VertexAttribute::Tangent;
        if (hasColors)
            mask |= VertexAttribute::Color;
        if (hasTexCoords)
            mask |= VertexAttribute::TexCoord0;
        const GpuVertexLayout layout = MakeVertexLayout(mask);

        std::vector<glm::vec4> generated;
        if (wantTangent && !hasTangents)
            generated = GenerateTangents(mesh);
        const std::vector<glm::vec4>* tangents = hasTangents ? &mesh.tangents : (wantTangent ? &generated : nullptr);

        // Interleave only the present attributes, tightly packed at layout.stride, via memcpy
        // (offsets are 4-byte aligned but not necessarily naturally aligned for a float write).
        std::vector<uint8_t> vertices(static_cast<size_t>(vcount) * layout.stride);
        for (size_t i = 0; i < vcount; ++i)
        {
            uint8_t* base = vertices.data() + i * layout.stride;
            for (const GpuVertexAttribute& a : layout.attributes)
            {
                uint8_t* dst = base + a.offset;
                switch (a.attribute)
                {
                    case VertexAttribute::Position: {
                        const float v[3] = {mesh.positions[i].x, mesh.positions[i].y, mesh.positions[i].z};
                        std::memcpy(dst, v, sizeof(v));
                        break;
                    }
                    case VertexAttribute::Normal: {
                        const float v[3] = {mesh.normals[i].x, mesh.normals[i].y, mesh.normals[i].z};
                        std::memcpy(dst, v, sizeof(v));
                        break;
                    }
                    case VertexAttribute::Tangent: {
                        const glm::vec4& t    = (*tangents)[i];
                        const float      v[4] = {t.x, t.y, t.z, t.w};
                        std::memcpy(dst, v, sizeof(v));
                        break;
                    }
                    case VertexAttribute::Color: {
                        const float v[4] = {mesh.colors[i].x, mesh.colors[i].y, mesh.colors[i].z, mesh.colors[i].w};
                        std::memcpy(dst, v, sizeof(v));
                        break;
                    }
                    case VertexAttribute::TexCoord0: {
                        const float v[2] = {mesh.texCoords0[i].x, mesh.texCoords0[i].y};
                        std::memcpy(dst, v, sizeof(v));
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        const bool     hasIndices = !mesh.indices.empty();
        const uint64_t vbSize     = vertices.size();
        const uint64_t ibSize     = mesh.indices.size() * sizeof(uint32_t);

        const VriBufferUsageFlags rtUsage = rtAccelInput ? VriBufferUsage_AccelerationBuildInput : 0;
        auto vertexBuffer                 = CreateDeviceBuffer(device, vbSize, VriBufferUsage_VertexBuffer | rtUsage);
        if (!vertexBuffer)
            return std::unexpected(vertexBuffer.error());
        VriBuffer* indexBuffer = nullptr;
        if (hasIndices)
        {
            auto ib = CreateDeviceBuffer(device, ibSize, VriBufferUsage_IndexBuffer | rtUsage);
            if (!ib)
            {
                c.DestroyBuffer(*vertexBuffer);
                return std::unexpected(ib.error());
            }
            indexBuffer = *ib;
        }

        auto vertexStaging = CreateStagingBuffer(device, vertices.data(), vbSize);
        if (!vertexStaging)
            return std::unexpected(vertexStaging.error());
        VriBuffer* indexStaging = nullptr;
        if (hasIndices)
        {
            auto is = CreateStagingBuffer(device, mesh.indices.data(), ibSize);
            if (!is)
                return std::unexpected(is.error());
            indexStaging = *is;
        }

        ImmediateSubmit(device, [&](VriCommandBuffer* cmd) {
            VriBufferCopyDesc vbCopy {};
            vbCopy.size = vbSize;
            c.CmdCopyBuffer(cmd, *vertexBuffer, *vertexStaging, &vbCopy);
            if (hasIndices)
            {
                VriBufferCopyDesc ibCopy {};
                ibCopy.size = ibSize;
                c.CmdCopyBuffer(cmd, indexBuffer, indexStaging, &ibCopy);
            }

            VriBufferBarrierDesc barriers[2] {};
            barriers[0].buffer        = *vertexBuffer;
            barriers[0].before.access = VriAccess_CopyDestinationWrite;
            barriers[0].before.stages = VriPipelineStage_Transfer;
            barriers[0].after.access  = VriAccess_VertexBufferRead;
            barriers[0].after.stages  = VriPipelineStage_VertexInput;
            uint32_t barrierNum       = 1;
            if (hasIndices)
            {
                barriers[1].buffer        = indexBuffer;
                barriers[1].before.access = VriAccess_CopyDestinationWrite;
                barriers[1].before.stages = VriPipelineStage_Transfer;
                barriers[1].after.access  = VriAccess_IndexBufferRead;
                barriers[1].after.stages  = VriPipelineStage_VertexInput;
                barrierNum                = 2;
            }
            VriBarrierGroupDesc group {};
            group.buffers   = barriers;
            group.bufferNum = barrierNum;
            c.CmdBarrier(cmd, &group);
        });

        c.DestroyBuffer(*vertexStaging);
        if (indexStaging)
            c.DestroyBuffer(indexStaging);

        GpuMesh out;
        out.vertexBuffer = *vertexBuffer;
        out.indexBuffer  = indexBuffer;
        out.vertexCount  = static_cast<uint32_t>(vcount);
        out.indexCount   = static_cast<uint32_t>(mesh.indices.size());
        out.indexType    = VriIndexType_UInt32;
        out.layout       = layout;
        return out;
    }

    Expected<GpuTexture> UploadTexture(RenderDevice& device, const Texture& texture)
    {
        if (texture.Empty())
            return MakeError("UploadTexture: texture has no data");
        if (texture.format == VriFormat_Unknown)
            return MakeError("UploadTexture: texture format is Unknown");
        if (texture.depth > 1)
            return MakeError("UploadTexture: 3D/volume textures are not yet supported");

        const VriCoreInterface& c     = device.Core();
        const FormatBlockInfo   block = GetFormatBlockInfo(texture.format);

        const uint32_t mipLevels   = texture.mipLevels > 0 ? texture.mipLevels : 1;
        const uint32_t arrayLayers = texture.arrayLayers > 0 ? texture.arrayLayers : 1;
        const bool     isArray     = arrayLayers > (texture.isCubemap ? 6u : 1u);

        // Subresource list (fall back to a single mip-0 / layer-0 image when the loader
        // didn't provide one - the stb / glTF fast path).
        std::vector<TextureSubresource> subresources = texture.subresources;
        if (subresources.empty())
        {
            TextureSubresource single;
            single.mipLevel   = 0;
            single.arrayLayer = 0;
            single.offset     = 0;
            single.size       = texture.data.size();
            single.width      = texture.width;
            single.height     = texture.height;
            subresources.push_back(single);
        }

        // ---- create the destination texture with the full mip/layer/cube shape ----
        VriTextureDesc td {};
        if (texture.isCubemap)
            td.type = isArray ? VriTextureType_CubeArray : VriTextureType_Cube;
        else
            td.type = isArray ? VriTextureType_2DArray : VriTextureType_2D;
        td.format              = texture.format;
        td.width               = texture.width;
        td.height              = texture.height;
        td.depth               = 1;
        td.mipNum              = mipLevels;
        td.layerNum            = arrayLayers;
        td.sampleNum           = 1;
        td.usage               = VriTextureUsage_ShaderResource | VriTextureUsage_TransferDst;
        td.memoryLocation      = VriMemoryLocation_Device;
        VriTexture* gpuTexture = nullptr;
        if (c.CreateTexture(device.Handle(), &td, &gpuTexture) != VriResult_Success)
            return MakeError("UploadTexture: CreateTexture failed");

        // ---- pack every subresource into one staging buffer with aligned offsets ----
        // Vulkan requires a copy's bufferOffset to be a multiple of the texel-block size (and 4).
        const uint64_t copyAlignment = block.blockBytes < 4 ? 4 : block.blockBytes;
        struct StagedCopy
        {
            uint64_t stagingOffset;
            uint32_t mipLevel;
            uint32_t arrayLayer;
            uint32_t width;
            uint32_t height;
            uint32_t bufferRowLength; // in texels; 0 = tightly packed
        };
        std::vector<StagedCopy> copies;
        copies.reserve(subresources.size());
        uint64_t stagingSize = 0;
        for (const TextureSubresource& sub : subresources)
        {
            const uint64_t offset = AlignUp(stagingSize, copyAlignment);
            uint32_t       rowLen = 0;
            if (sub.rowPitchBytes > 0 && block.blockBytes > 0)
                rowLen = (sub.rowPitchBytes / block.blockBytes) * block.blockWidth;
            copies.push_back({offset, sub.mipLevel, sub.arrayLayer, sub.width, sub.height, rowLen});
            stagingSize = offset + sub.size;
        }

        VriBufferDesc sd {};
        sd.size            = stagingSize;
        sd.usage           = VriBufferUsage_TransferSrc;
        sd.memoryLocation  = VriMemoryLocation_HostUpload;
        VriBuffer* staging = nullptr;
        if (c.CreateBuffer(device.Handle(), &sd, &staging) != VriResult_Success)
        {
            c.DestroyTexture(gpuTexture);
            return MakeError("UploadTexture: staging CreateBuffer failed");
        }
        {
            auto* mapped = static_cast<uint8_t*>(c.MapBuffer(staging, 0, stagingSize));
            for (size_t i = 0; i < subresources.size(); ++i)
                std::memcpy(mapped + copies[i].stagingOffset,
                            texture.data.data() + subresources[i].offset,
                            subresources[i].size);
            c.UnmapBuffer(staging);
        }

        ImmediateSubmit(device, [&](VriCommandBuffer* cmd) {
            // Undefined -> CopyDestination for every mip/layer (0 = all).
            VriTextureBarrierDesc toCopy {};
            toCopy.texture       = gpuTexture;
            toCopy.before.layout = VriLayout_Undefined;
            toCopy.before.stages = VriPipelineStage_None;
            toCopy.after.access  = VriAccess_CopyDestinationWrite;
            toCopy.after.layout  = VriLayout_CopyDestination;
            toCopy.after.stages  = VriPipelineStage_Transfer;
            toCopy.aspect        = VriImageAspect_Color;
            VriBarrierGroupDesc toCopyGroup {};
            toCopyGroup.textures   = &toCopy;
            toCopyGroup.textureNum = 1;
            c.CmdBarrier(cmd, &toCopyGroup);

            for (const StagedCopy& sc : copies)
            {
                VriBufferTextureCopyDesc copy {};
                copy.bufferOffset      = sc.stagingOffset;
                copy.bufferRowLength   = sc.bufferRowLength;
                copy.texture.aspect    = VriImageAspect_Color;
                copy.texture.mip       = sc.mipLevel;
                copy.texture.baseLayer = sc.arrayLayer;
                copy.texture.layerNum  = 1;
                copy.texture.width     = sc.width;
                copy.texture.height    = sc.height;
                c.CmdUploadBufferToTexture(cmd, gpuTexture, staging, &copy);
            }

            VriTextureBarrierDesc toSampled {};
            toSampled.texture       = gpuTexture;
            toSampled.before.access = VriAccess_CopyDestinationWrite;
            toSampled.before.layout = VriLayout_CopyDestination;
            toSampled.before.stages = VriPipelineStage_Transfer;
            toSampled.after.access  = VriAccess_ShaderResourceRead;
            toSampled.after.layout  = VriLayout_ShaderResource;
            toSampled.after.stages  = VriPipelineStage_FragmentShader;
            toSampled.aspect        = VriImageAspect_Color;
            VriBarrierGroupDesc toSampledGroup {};
            toSampledGroup.textures   = &toSampled;
            toSampledGroup.textureNum = 1;
            c.CmdBarrier(cmd, &toSampledGroup);
        });

        c.DestroyBuffer(staging);

        VriTextureViewDesc vd {};
        vd.texture = gpuTexture;
        if (texture.isCubemap)
            vd.viewType = isArray ? VriTextureViewType_CubeArray : VriTextureViewType_Cube;
        else
            vd.viewType = isArray ? VriTextureViewType_2DArray : VriTextureViewType_2D;
        vd.format           = VriFormat_Unknown;
        vd.aspect           = VriImageAspect_Color;
        VriDescriptor* view = nullptr;
        if (c.CreateTextureView(device.Handle(), &vd, &view) != VriResult_Success)
        {
            c.DestroyTexture(gpuTexture);
            return MakeError("UploadTexture: CreateTextureView failed");
        }

        GpuTexture out;
        out.texture = gpuTexture;
        out.view    = view;
        return out;
    }
} // namespace vrf
