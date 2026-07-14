/*
 * raytracing.hpp - acceleration structures + ray-tracing pipeline over VRI's
 * ray tracing extension (vri_ext_raytracing.h).
 *
 * Gate on VriDeviceDesc::hasRayTracing and request VriFeature_RayTracing at
 * device creation. Blas/Tlas split creation (sizing) from CmdBuild (recording),
 * so a TLAS can be rebuilt per frame for dynamic scenes; the pipeline builder
 * owns the shader binding table.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vri/ext/vri_ext_raytracing.h>
#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"
#include "vrf/fg/render_context.hpp" // PipelineLayoutInfo

namespace vrf
{
    class RenderDevice;

    // Cached VRI ray-tracing function table for a device.
    class RayTracing
    {
    public:
        [[nodiscard]] static Expected<RayTracing> Get(RenderDevice&);

        [[nodiscard]] const VriRayTracingInterface& Api() const noexcept { return m_api; }

    private:
        VriRayTracingInterface m_api {};
    };

    // One TLAS instance record (VkAccelerationStructureInstanceKHR layout).
    struct AsInstance
    {
        float    transform[12] {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; // 3x4 row-major
        uint32_t instanceIdAndMask {0xFFu << 24};
        uint32_t sbtOffsetAndFlags {0};
        uint64_t blasReference {0};

        // From a column-major glm transform + a BLAS device address.
        [[nodiscard]] static AsInstance Make(const Mat4& transform,
                                             uint64_t    blasAddress,
                                             uint32_t    instanceId = 0,
                                             uint8_t     mask       = 0xFF,
                                             uint32_t    sbtOffset  = 0,
                                             uint8_t     flags      = 0x1 /* cull-disable */);
    };

    class Blas
    {
    public:
        Blas() = default;
        ~Blas();
        Blas(const Blas&)            = delete;
        Blas& operator=(const Blas&) = delete;
        Blas(Blas&& other) noexcept;
        Blas& operator=(Blas&& other) noexcept;

        // Size an AS for the given triangle geometries (buffers must carry
        // VriBufferUsage_AccelerationBuildInput). Record the build with CmdBuild.
        [[nodiscard]] static Expected<Blas>
        Create(RenderDevice&,
               std::vector<VriAsTrianglesDesc>    geometries,
               VriAsGeometryFlags                 geometryFlags = VriAsGeometry_Opaque,
               VriAccelerationStructureBuildFlags buildFlags    = VriAccelerationStructureBuild_PreferFastTrace);

        void CmdBuild(VriCommandBuffer*);

        [[nodiscard]] uint64_t                  DeviceAddress() const;
        [[nodiscard]] VriAccelerationStructure* Handle() const noexcept { return m_as; }
        [[nodiscard]] explicit                  operator bool() const noexcept { return m_as != nullptr; }

    private:
        void Reset() noexcept;

        RenderDevice*             m_device = nullptr;
        VriRayTracingInterface    m_api {};
        VriAccelerationStructure* m_as = nullptr;

        std::vector<VriAsGeometryDesc> m_geometries;
        VriAccelerationStructureDesc   m_desc {};
    };

    class Tlas
    {
    public:
        Tlas() = default;
        ~Tlas();
        Tlas(const Tlas&)            = delete;
        Tlas& operator=(const Tlas&) = delete;
        Tlas(Tlas&& other) noexcept;
        Tlas& operator=(Tlas&& other) noexcept;

        // Sized for up to maxInstances; owns a host-visible instance buffer so
        // SetInstances + CmdBuild can run every frame.
        [[nodiscard]] static Expected<Tlas>
        Create(RenderDevice&,
               uint32_t                           maxInstances,
               VriAccelerationStructureBuildFlags buildFlags = VriAccelerationStructureBuild_PreferFastTrace);

        // Map-write the instance records (count <= maxInstances).
        void SetInstances(const AsInstance* instances, uint32_t count);

        void CmdBuild(VriCommandBuffer*);

        // Descriptor for binding (VriDescriptorType_AccelerationStructure);
        // cached, destroyed with the Tlas.
        [[nodiscard]] VriDescriptor* Descriptor();

        [[nodiscard]] VriAccelerationStructure* Handle() const noexcept { return m_as; }
        [[nodiscard]] explicit                  operator bool() const noexcept { return m_as != nullptr; }

    private:
        void Reset() noexcept;

        RenderDevice*             m_device = nullptr;
        VriRayTracingInterface    m_api {};
        VriAccelerationStructure* m_as             = nullptr;
        VriBuffer*                m_instanceBuffer = nullptr;
        VriDescriptor*            m_descriptor     = nullptr;
        uint32_t                  m_maxInstances   = 0;

        VriAsGeometryDesc            m_geometry {};
        VriAccelerationStructureDesc m_desc {};
    };

    // A ray-tracing pipeline + its shader binding table, dispatchable with
    // CmdTraceRays. Build via RayTracingPipelineBuilder.
    class RayTracingPipeline
    {
    public:
        RayTracingPipeline() = default;
        ~RayTracingPipeline();
        RayTracingPipeline(const RayTracingPipeline&)            = delete;
        RayTracingPipeline& operator=(const RayTracingPipeline&) = delete;
        RayTracingPipeline(RayTracingPipeline&& other) noexcept;
        RayTracingPipeline& operator=(RayTracingPipeline&& other) noexcept;

        void CmdTraceRays(VriCommandBuffer*, uint32_t width, uint32_t height, uint32_t depth = 1) const;

        [[nodiscard]] VriPipeline*                                   Handle() const noexcept { return m_pipeline; }
        [[nodiscard]] const std::shared_ptr<fg::PipelineLayoutInfo>& Layout() const noexcept { return m_layout; }
        [[nodiscard]] explicit operator bool() const noexcept { return m_pipeline != nullptr; }

    private:
        friend class RayTracingPipelineBuilder;
        void Reset() noexcept;

        RenderDevice*          m_device = nullptr;
        VriRayTracingInterface m_api {};
        VriPipeline*           m_pipeline = nullptr;
        VriBuffer*             m_sbt      = nullptr;

        std::shared_ptr<fg::PipelineLayoutInfo> m_layout;

        VriStridedBufferRegion m_raygenRegion {};
        VriStridedBufferRegion m_missRegion {};
        VriStridedBufferRegion m_hitRegion {};
    };

    class RayTracingPipelineBuilder
    {
    public:
        struct Shader
        {
            const void* spirv {nullptr};
            size_t      size {0};
            const char* entryPoint {"main"};
        };

        RayTracingPipelineBuilder& SetPipelineLayout(std::shared_ptr<fg::PipelineLayoutInfo> layout)
        {
            m_layout = std::move(layout);
            return *this;
        }

        RayTracingPipelineBuilder& SetRayGen(const Shader&);
        RayTracingPipelineBuilder& AddMiss(const Shader&);
        RayTracingPipelineBuilder& AddHitGroup(const Shader& closestHit, const Shader* anyHit = nullptr);

        RayTracingPipelineBuilder& SetMaxRecursionDepth(uint32_t depth)
        {
            m_maxRecursionDepth = depth;
            return *this;
        }

        [[nodiscard]] Expected<RayTracingPipeline> Build(RenderDevice&) const;

    private:
        std::shared_ptr<fg::PipelineLayoutInfo> m_layout;
        std::vector<VriShaderDesc>              m_shaders;
        std::vector<VriShaderGroupDesc>         m_groups;
        uint32_t                                m_raygenGroups {0};
        uint32_t                                m_missGroups {0};
        uint32_t                                m_maxRecursionDepth {1};
    };
} // namespace vrf
