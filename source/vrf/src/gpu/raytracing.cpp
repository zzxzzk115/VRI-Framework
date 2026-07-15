#include "vrf/gpu/raytracing.hpp"

#include <cassert>
#include <cstring>
#include <utility>

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    // ---- RayTracing (interface table) -------------------------------------

    Expected<RayTracing> RayTracing::Get(RenderDevice& device)
    {
        // Acceleration structures back BOTH the SBT ray-tracing pipeline
        // (hasRayTracing) and inline ray query (hasRayQuery) - the latter is all
        // the native Metal backend exposes (no DXR-style SBT). Allow either so
        // Blas/Tlas build for inline-RT-only devices too.
        if (device.Desc()->hasRayTracing == VRI_FALSE && device.Desc()->hasRayQuery == VRI_FALSE)
        {
            return MakeError(VriResult_Unsupported, "RayTracing::Get", "device has no ray tracing or ray query");
        }
        RayTracing out;
        if (const auto r = vriGetInterface(device.Handle(), VRI_INTERFACE_RAYTRACING, sizeof(out.m_api), &out.m_api);
            !Succeeded(r))
        {
            return MakeError(r, "RayTracing::Get", "vriGetInterface(RAYTRACING) failed");
        }
        return out;
    }

    // ---- AsInstance ----------------------------------------------------------

    AsInstance AsInstance::Make(const Mat4&    transform,
                                const uint64_t blasAddress,
                                const uint32_t instanceId,
                                const uint8_t  mask,
                                const uint32_t sbtOffset,
                                const uint8_t  flags)
    {
        AsInstance out;
        // glm is column-major; the record wants a 3x4 row-major matrix.
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                out.transform[row * 4 + col] = transform[col][row];
            }
        }
        out.instanceIdAndMask = (instanceId & 0xFFFFFFu) | (uint32_t {mask} << 24);
        out.sbtOffsetAndFlags = (sbtOffset & 0xFFFFFFu) | (uint32_t {flags} << 24);
        out.blasReference     = blasAddress;
        return out;
    }

    // ---- Blas -----------------------------------------------------------------

    Blas::~Blas() { Reset(); }

    Blas::Blas(Blas&& other) noexcept :
        m_device {std::exchange(other.m_device, nullptr)}, m_api {other.m_api},
        m_as {std::exchange(other.m_as, nullptr)}, m_geometries {std::move(other.m_geometries)}, m_desc {other.m_desc}
    {
        m_desc.geometries = m_geometries.data();
    }

    Blas& Blas::operator=(Blas&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device          = std::exchange(other.m_device, nullptr);
            m_api             = other.m_api;
            m_as              = std::exchange(other.m_as, nullptr);
            m_geometries      = std::move(other.m_geometries);
            m_desc            = other.m_desc;
            m_desc.geometries = m_geometries.data();
        }
        return *this;
    }

    Expected<Blas> Blas::Create(RenderDevice&                            device,
                                std::vector<VriAsTrianglesDesc>          geometries,
                                const VriAsGeometryFlags                 geometryFlags,
                                const VriAccelerationStructureBuildFlags buildFlags)
    {
        auto api = RayTracing::Get(device);
        if (!api)
        {
            return std::unexpected(api.error());
        }

        Blas out;
        out.m_device = &device;
        out.m_api    = api->Api();

        out.m_geometries.reserve(geometries.size());
        for (const auto& triangles : geometries)
        {
            out.m_geometries.push_back(VriAsGeometryDesc {
                .type      = VriAsGeometryType_Triangles,
                .flags     = geometryFlags,
                .triangles = triangles,
            });
        }
        out.m_desc = VriAccelerationStructureDesc {
            .type          = VriAccelerationStructureType_BottomLevel,
            .flags         = buildFlags,
            .geometryCount = static_cast<uint32_t>(out.m_geometries.size()),
            .geometries    = out.m_geometries.data(),
        };

        if (const auto r = out.m_api.CreateAccelerationStructure(device.Handle(), &out.m_desc, &out.m_as);
            !Succeeded(r))
        {
            return MakeError(r, "Blas::Create", "CreateAccelerationStructure failed");
        }
        return out;
    }

    Expected<Blas> Blas::CreateAabbs(RenderDevice&                            device,
                                     std::vector<VriAsAabbsDesc>              geometries,
                                     const VriAsGeometryFlags                 geometryFlags,
                                     const VriAccelerationStructureBuildFlags buildFlags)
    {
        auto api = RayTracing::Get(device);
        if (!api)
        {
            return std::unexpected(api.error());
        }

        Blas out;
        out.m_device = &device;
        out.m_api    = api->Api();

        out.m_geometries.reserve(geometries.size());
        for (const auto& aabbs : geometries)
        {
            out.m_geometries.push_back(VriAsGeometryDesc {
                .type  = VriAsGeometryType_Aabbs,
                .flags = geometryFlags,
                .aabbs = aabbs,
            });
        }
        out.m_desc = VriAccelerationStructureDesc {
            .type          = VriAccelerationStructureType_BottomLevel,
            .flags         = buildFlags,
            .geometryCount = static_cast<uint32_t>(out.m_geometries.size()),
            .geometries    = out.m_geometries.data(),
        };

        if (const auto r = out.m_api.CreateAccelerationStructure(device.Handle(), &out.m_desc, &out.m_as);
            !Succeeded(r))
        {
            return MakeError(r, "Blas::CreateAabbs", "CreateAccelerationStructure failed");
        }
        return out;
    }

    void Blas::CmdBuild(VriCommandBuffer* cmd)
    {
        assert(m_as);
        const VriBuildAccelerationStructureDesc build {.dst = m_as, .geometry = &m_desc};
        m_api.CmdBuildAccelerationStructure(cmd, &build);
    }

    uint64_t Blas::DeviceAddress() const
    {
        assert(m_as);
        return m_api.GetAccelerationStructureDeviceAddress(m_as);
    }

    void Blas::Reset() noexcept
    {
        if (m_as)
        {
            m_api.DestroyAccelerationStructure(m_as);
            m_as = nullptr;
        }
        m_device = nullptr;
    }

    // ---- Tlas -----------------------------------------------------------------

    Tlas::~Tlas() { Reset(); }

    Tlas::Tlas(Tlas&& other) noexcept :
        m_device {std::exchange(other.m_device, nullptr)}, m_api {other.m_api},
        m_as {std::exchange(other.m_as, nullptr)}, m_instanceBuffer {std::exchange(other.m_instanceBuffer, nullptr)},
        m_descriptor {std::exchange(other.m_descriptor, nullptr)}, m_maxInstances {other.m_maxInstances},
        m_geometry {other.m_geometry}, m_desc {other.m_desc}
    {
        m_desc.geometries = &m_geometry;
    }

    Tlas& Tlas::operator=(Tlas&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device          = std::exchange(other.m_device, nullptr);
            m_api             = other.m_api;
            m_as              = std::exchange(other.m_as, nullptr);
            m_instanceBuffer  = std::exchange(other.m_instanceBuffer, nullptr);
            m_descriptor      = std::exchange(other.m_descriptor, nullptr);
            m_maxInstances    = other.m_maxInstances;
            m_geometry        = other.m_geometry;
            m_desc            = other.m_desc;
            m_desc.geometries = &m_geometry;
        }
        return *this;
    }

    Expected<Tlas>
    Tlas::Create(RenderDevice& device, const uint32_t maxInstances, const VriAccelerationStructureBuildFlags buildFlags)
    {
        assert(maxInstances > 0);
        auto api = RayTracing::Get(device);
        if (!api)
        {
            return std::unexpected(api.error());
        }

        Tlas out;
        out.m_device       = &device;
        out.m_api          = api->Api();
        out.m_maxInstances = maxInstances;

        const VriBufferDesc bufferDesc {
            .size           = sizeof(AsInstance) * maxInstances,
            .usage          = VriBufferUsage_AccelerationBuildInput,
            .memoryLocation = VriMemoryLocation_HostUpload,
        };
        if (const auto r = device.Core().CreateBuffer(device.Handle(), &bufferDesc, &out.m_instanceBuffer);
            !Succeeded(r))
        {
            return MakeError(r, "Tlas::Create", "instance buffer creation failed");
        }

        out.m_geometry = VriAsGeometryDesc {
            .type      = VriAsGeometryType_Instances,
            .instances = {.instanceBuffer = out.m_instanceBuffer, .offset = 0, .instanceCount = maxInstances},
        };
        out.m_desc = VriAccelerationStructureDesc {
            .type          = VriAccelerationStructureType_TopLevel,
            .flags         = buildFlags,
            .geometryCount = 1,
            .geometries    = &out.m_geometry,
        };

        if (const auto r = out.m_api.CreateAccelerationStructure(device.Handle(), &out.m_desc, &out.m_as);
            !Succeeded(r))
        {
            return MakeError(r, "Tlas::Create", "CreateAccelerationStructure failed");
        }
        return out;
    }

    void Tlas::SetInstances(const AsInstance* instances, const uint32_t count)
    {
        assert(count <= m_maxInstances);
        void* dst = m_device->Core().MapBuffer(m_instanceBuffer, 0, sizeof(AsInstance) * count);
        std::memcpy(dst, instances, sizeof(AsInstance) * count);
        m_device->Core().UnmapBuffer(m_instanceBuffer);
        m_geometry.instances.instanceCount = count;
    }

    void Tlas::CmdBuild(VriCommandBuffer* cmd)
    {
        assert(m_as);
        const VriBuildAccelerationStructureDesc build {.dst = m_as, .geometry = &m_desc};
        m_api.CmdBuildAccelerationStructure(cmd, &build);
    }

    VriDescriptor* Tlas::Descriptor()
    {
        if (!m_descriptor)
        {
            if (const auto r = m_api.CreateAccelerationStructureDescriptor(m_device->Handle(), m_as, &m_descriptor);
                !Succeeded(r))
            {
                return nullptr;
            }
        }
        return m_descriptor;
    }

    void Tlas::Reset() noexcept
    {
        if (m_device)
        {
            if (m_descriptor)
            {
                m_device->Core().DestroyDescriptor(m_descriptor);
            }
            if (m_as)
            {
                m_api.DestroyAccelerationStructure(m_as);
            }
            if (m_instanceBuffer)
            {
                m_device->Core().DestroyBuffer(m_instanceBuffer);
            }
        }
        m_descriptor     = nullptr;
        m_as             = nullptr;
        m_instanceBuffer = nullptr;
        m_device         = nullptr;
    }

    // ---- RayTracingPipeline ----------------------------------------------------

    RayTracingPipeline::~RayTracingPipeline() { Reset(); }

    RayTracingPipeline::RayTracingPipeline(RayTracingPipeline&& other) noexcept :
        m_device {std::exchange(other.m_device, nullptr)}, m_api {other.m_api},
        m_pipeline {std::exchange(other.m_pipeline, nullptr)}, m_sbt {std::exchange(other.m_sbt, nullptr)},
        m_layout {std::move(other.m_layout)}, m_raygenRegion {other.m_raygenRegion}, m_missRegion {other.m_missRegion},
        m_hitRegion {other.m_hitRegion}
    {}

    RayTracingPipeline& RayTracingPipeline::operator=(RayTracingPipeline&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device       = std::exchange(other.m_device, nullptr);
            m_api          = other.m_api;
            m_pipeline     = std::exchange(other.m_pipeline, nullptr);
            m_sbt          = std::exchange(other.m_sbt, nullptr);
            m_layout       = std::move(other.m_layout);
            m_raygenRegion = other.m_raygenRegion;
            m_missRegion   = other.m_missRegion;
            m_hitRegion    = other.m_hitRegion;
        }
        return *this;
    }

    void RayTracingPipeline::CmdTraceRays(VriCommandBuffer* cmd,
                                          const uint32_t    width,
                                          const uint32_t    height,
                                          const uint32_t    depth) const
    {
        assert(m_pipeline);
        const VriDispatchRaysDesc trace {
            .raygen = m_raygenRegion,
            .miss   = m_missRegion,
            .hit    = m_hitRegion,
            .width  = width,
            .height = height,
            .depth  = depth,
        };
        m_api.CmdTraceRays(cmd, &trace);
    }

    void RayTracingPipeline::Reset() noexcept
    {
        if (m_device)
        {
            if (m_pipeline)
            {
                m_device->Core().DestroyPipeline(m_pipeline);
            }
            if (m_sbt)
            {
                m_device->Core().DestroyBuffer(m_sbt);
            }
        }
        m_pipeline = nullptr;
        m_sbt      = nullptr;
        m_device   = nullptr;
    }

    // ---- RayTracingPipelineBuilder ----------------------------------------------

    RayTracingPipelineBuilder& RayTracingPipelineBuilder::SetRayGen(const Shader& shader)
    {
        assert(m_raygenGroups == 0 && m_missGroups == 0 && "raygen must be added first");
        m_shaders.push_back(VriShaderDesc {VriShaderStage_RayGen, shader.spirv, shader.size, shader.entryPoint});
        m_groups.push_back(VriShaderGroupDesc {
            .type               = VriShaderGroupType_General,
            .generalShader      = static_cast<uint32_t>(m_shaders.size() - 1),
            .closestHitShader   = VRI_SHADER_UNUSED,
            .anyHitShader       = VRI_SHADER_UNUSED,
            .intersectionShader = VRI_SHADER_UNUSED,
        });
        ++m_raygenGroups;
        return *this;
    }

    RayTracingPipelineBuilder& RayTracingPipelineBuilder::AddMiss(const Shader& shader)
    {
        m_shaders.push_back(VriShaderDesc {VriShaderStage_Miss, shader.spirv, shader.size, shader.entryPoint});
        m_groups.push_back(VriShaderGroupDesc {
            .type               = VriShaderGroupType_General,
            .generalShader      = static_cast<uint32_t>(m_shaders.size() - 1),
            .closestHitShader   = VRI_SHADER_UNUSED,
            .anyHitShader       = VRI_SHADER_UNUSED,
            .intersectionShader = VRI_SHADER_UNUSED,
        });
        ++m_missGroups;
        return *this;
    }

    RayTracingPipelineBuilder& RayTracingPipelineBuilder::AddHitGroup(const Shader& closestHit, const Shader* anyHit)
    {
        m_shaders.push_back(
            VriShaderDesc {VriShaderStage_ClosestHit, closestHit.spirv, closestHit.size, closestHit.entryPoint});
        const auto closestHitIndex = static_cast<uint32_t>(m_shaders.size() - 1);

        uint32_t anyHitIndex = VRI_SHADER_UNUSED;
        if (anyHit)
        {
            m_shaders.push_back(VriShaderDesc {VriShaderStage_AnyHit, anyHit->spirv, anyHit->size, anyHit->entryPoint});
            anyHitIndex = static_cast<uint32_t>(m_shaders.size() - 1);
        }

        m_groups.push_back(VriShaderGroupDesc {
            .type               = VriShaderGroupType_TrianglesHitGroup,
            .generalShader      = VRI_SHADER_UNUSED,
            .closestHitShader   = closestHitIndex,
            .anyHitShader       = anyHitIndex,
            .intersectionShader = VRI_SHADER_UNUSED,
        });
        return *this;
    }

    Expected<RayTracingPipeline> RayTracingPipelineBuilder::Build(RenderDevice& device) const
    {
        assert(m_layout && m_layout->handle);
        assert(m_raygenGroups == 1 && "exactly one raygen group required");

        auto api = RayTracing::Get(device);
        if (!api)
        {
            return std::unexpected(api.error());
        }

        RayTracingPipeline out;
        out.m_device = &device;
        out.m_api    = api->Api();
        out.m_layout = m_layout;

        const VriRayTracingPipelineDesc desc {
            .pipelineLayout    = m_layout->handle,
            .shaders           = m_shaders.data(),
            .shaderNum         = static_cast<uint32_t>(m_shaders.size()),
            .groups            = m_groups.data(),
            .groupNum          = static_cast<uint32_t>(m_groups.size()),
            .maxRecursionDepth = m_maxRecursionDepth,
        };
        if (const auto r = out.m_api.CreateRayTracingPipeline(device.Handle(), &desc, &out.m_pipeline); !Succeeded(r))
        {
            return MakeError(r, "RayTracingPipelineBuilder::Build", "CreateRayTracingPipeline failed");
        }

        // ---- shader binding table: one baseAlignment-strided record per group,
        // ordered raygen | miss... | hit... --------------------------------------
        const uint32_t handleSize = device.Desc()->rtShaderGroupHandleSize;
        const uint32_t baseAlign  = device.Desc()->rtShaderGroupBaseAlignment;
        const auto     groupCount = static_cast<uint32_t>(m_groups.size());

        std::vector<uint8_t> handles(static_cast<size_t>(groupCount) * handleSize);
        if (const auto r =
                out.m_api.GetShaderGroupHandles(out.m_pipeline, 0, groupCount, handles.size(), handles.data());
            !Succeeded(r))
        {
            return MakeError(r, "RayTracingPipelineBuilder::Build", "GetShaderGroupHandles failed");
        }

        const VriBufferDesc sbtDesc {
            .size           = static_cast<uint64_t>(baseAlign) * groupCount,
            .usage          = VriBufferUsage_ShaderBindingTable,
            .memoryLocation = VriMemoryLocation_HostUpload,
        };
        if (const auto r = device.Core().CreateBuffer(device.Handle(), &sbtDesc, &out.m_sbt); !Succeeded(r))
        {
            return MakeError(r, "RayTracingPipelineBuilder::Build", "SBT buffer creation failed");
        }
        auto* mapped = static_cast<uint8_t*>(device.Core().MapBuffer(out.m_sbt, 0, sbtDesc.size));
        std::memset(mapped, 0, sbtDesc.size);
        for (uint32_t i = 0; i < groupCount; ++i)
        {
            std::memcpy(mapped + static_cast<uint64_t>(i) * baseAlign,
                        handles.data() + static_cast<size_t>(i) * handleSize,
                        handleSize);
        }
        device.Core().UnmapBuffer(out.m_sbt);

        const auto region = [&](const uint32_t firstGroup, const uint32_t count) {
            return VriStridedBufferRegion {
                .buffer = out.m_sbt,
                .offset = static_cast<uint64_t>(firstGroup) * baseAlign,
                .stride = baseAlign,
                .size   = static_cast<uint64_t>(count) * baseAlign,
            };
        };
        const uint32_t hitGroups = groupCount - m_raygenGroups - m_missGroups;
        out.m_raygenRegion       = region(0, m_raygenGroups);
        out.m_missRegion         = region(m_raygenGroups, m_missGroups);
        out.m_hitRegion          = region(m_raygenGroups + m_missGroups, hitGroups);
        return out;
    }
} // namespace vrf
