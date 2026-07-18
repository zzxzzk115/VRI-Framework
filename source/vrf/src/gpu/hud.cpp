#include "vrf/gpu/hud.hpp"

#include <cstring>
#include <utility>

#include <stb_easy_font.h>

#include "vrf/fg/render_context.hpp"
#include "vrf/gpu/builders/graphics_pipeline_builder.hpp"
#include "vrf/gpu/builders/pipeline_layout_builder.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    namespace
    {
        struct HudPush
        {
            glm::vec2 invTargetSize;
            glm::vec2 pad;
        };
        constexpr float kStbFontHeight = 7.0f; // stb_easy_font cell height at scale 1
    } // namespace

    Expected<Hud> Hud::Create(RenderDevice& device, Shader vertex, Shader fragment)
    {
        // Just a push constant (no descriptor sets - text is untextured colored triangles).
        PipelineLayoutBuilder layoutBuilder;
        layoutBuilder.SetShaderStages(VriShaderStage_AllGraphics);
        layoutBuilder.AddPushConstant(0, sizeof(HudPush), VriShaderStage_Vertex);
        auto layout = layoutBuilder.Build(device);
        if (!layout)
            return std::unexpected(layout.error());

        Hud hud;
        hud.m_device = &device;
        hud.m_layout = *layout;
        hud.m_vsSpirv.assign(static_cast<const uint32_t*>(vertex.spirv),
                             static_cast<const uint32_t*>(vertex.spirv) + vertex.size / sizeof(uint32_t));
        hud.m_fsSpirv.assign(static_cast<const uint32_t*>(fragment.spirv),
                             static_cast<const uint32_t*>(fragment.spirv) + fragment.size / sizeof(uint32_t));
        hud.m_vsEntry = vertex.entry;
        hud.m_fsEntry = fragment.entry;
        return hud;
    }

    VriPipeline* Hud::pipelineFor(VriFormat colorFormat)
    {
        for (const auto& fp : m_pipelines)
            if (fp.format == colorFormat)
                return fp.pipeline;

        VriBlendDesc blend {};
        blend.enable   = VRI_TRUE;
        blend.srcColor = VriBlendFactor_SrcAlpha;
        blend.dstColor = VriBlendFactor_OneMinusSrcAlpha;
        blend.colorOp  = VriBlendOp_Add;
        blend.srcAlpha = VriBlendFactor_One;
        blend.dstAlpha = VriBlendFactor_OneMinusSrcAlpha;
        blend.alphaOp  = VriBlendOp_Add;

        auto pipeline =
            GraphicsPipelineBuilder {}
                .SetPipelineLayout(m_layout)
                .AddShader(
                    VriShaderStage_Vertex, m_vsSpirv.data(), m_vsSpirv.size() * sizeof(uint32_t), m_vsEntry.c_str())
                .AddShader(
                    VriShaderStage_Fragment, m_fsSpirv.data(), m_fsSpirv.size() * sizeof(uint32_t), m_fsEntry.c_str())
                .SetTopology(VriPrimitiveTopology_TriangleList)
                .SetCullMode(VriCullMode_None)
                .AddVertexStream(sizeof(Vertex), 0)
                .AddVertexAttribute(VriFormat_RG32_SFLOAT, offsetof(Vertex, pos), 0)   // POSITION
                .AddVertexAttribute(VriFormat_RGBA32_SFLOAT, offsetof(Vertex, col), 0) // COLOR0
                .AddColorAttachment(colorFormat, VriColorWrite_RGBA, blend)
                .Build(*m_device);
        if (!pipeline)
            return nullptr;
        m_pipelines.push_back({colorFormat, *pipeline});
        return *pipeline;
    }

    Hud::Hud(Hud&& o) noexcept { *this = std::move(o); }

    Hud& Hud::operator=(Hud&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            m_device    = o.m_device;
            m_layout    = o.m_layout;
            m_vsSpirv   = std::move(o.m_vsSpirv);
            m_fsSpirv   = std::move(o.m_fsSpirv);
            m_vsEntry   = std::move(o.m_vsEntry);
            m_fsEntry   = std::move(o.m_fsEntry);
            m_pipelines = std::move(o.m_pipelines);
            m_vertices  = std::move(o.m_vertices);
            m_ring      = o.m_ring;
            for (uint32_t i = 0; i < kRing; ++i)
            {
                m_buffers[i]    = o.m_buffers[i];
                m_capacities[i] = o.m_capacities[i];
                o.m_buffers[i]  = nullptr;
            }
            o.m_device = nullptr;
            o.m_layout = nullptr;
        }
        return *this;
    }

    Hud::~Hud() { reset(); }

    void Hud::reset() noexcept
    {
        if (!m_device)
            return;
        const auto& core = m_device->Core();
        for (uint32_t i = 0; i < kRing; ++i)
            if (m_buffers[i])
                core.DestroyBuffer(m_buffers[i]);
        for (const auto& fp : m_pipelines)
            if (fp.pipeline)
                core.DestroyPipeline(fp.pipeline);
        if (m_layout)
            core.DestroyPipelineLayout(m_layout);
        m_device = nullptr;
    }

    void Hud::rect(float x, float y, float w, float h, const glm::vec4& color)
    {
        const glm::vec2 a {x, y}, b {x + w, y}, c {x + w, y + h}, d {x, y + h};
        m_vertices.push_back({a, color});
        m_vertices.push_back({b, color});
        m_vertices.push_back({c, color});
        m_vertices.push_back({a, color});
        m_vertices.push_back({c, color});
        m_vertices.push_back({d, color});
    }

    void Hud::text(const std::string& str, float x, float y, float scale, const glm::vec4& color)
    {
        if (str.empty())
            return;
        static thread_local std::vector<char> buffer;
        buffer.resize(str.size() * 270 + 512);
        const int quads = stb_easy_font_print(
            0.0f, 0.0f, const_cast<char*>(str.c_str()), nullptr, buffer.data(), static_cast<int>(buffer.size()));
        struct StbVert
        {
            float         x, y, z;
            unsigned char color[4];
        };
        const auto* verts = reinterpret_cast<const StbVert*>(buffer.data());
        auto        p     = [&](const StbVert& v) { return glm::vec2 {x + v.x * scale, y + v.y * scale}; };
        for (int q = 0; q < quads; ++q)
        {
            const StbVert& v0 = verts[q * 4 + 0];
            const StbVert& v1 = verts[q * 4 + 1];
            const StbVert& v2 = verts[q * 4 + 2];
            const StbVert& v3 = verts[q * 4 + 3];
            m_vertices.push_back({p(v0), color});
            m_vertices.push_back({p(v1), color});
            m_vertices.push_back({p(v2), color});
            m_vertices.push_back({p(v0), color});
            m_vertices.push_back({p(v2), color});
            m_vertices.push_back({p(v3), color});
        }
    }

    float Hud::textWidth(const std::string& str, float scale) const
    {
        return static_cast<float>(stb_easy_font_width(const_cast<char*>(str.c_str()))) * scale;
    }

    float Hud::textHeight(float scale) const { return kStbFontHeight * scale; }

    Hud::Batch Hud::upload()
    {
        if (!m_device || m_vertices.empty())
            return {};
        m_ring               = (m_ring + 1) % kRing;
        const uint32_t count = static_cast<uint32_t>(m_vertices.size());
        const auto&    core  = m_device->Core();
        const uint64_t bytes = m_vertices.size() * sizeof(Vertex);
        if (m_capacities[m_ring] < bytes)
        {
            if (m_buffers[m_ring])
                core.DestroyBuffer(m_buffers[m_ring]);
            const uint64_t      cap = bytes + bytes / 2 + 4096;
            const VriBufferDesc desc {.size            = cap,
                                      .structureStride = 0,
                                      .usage           = VriBufferUsage_VertexBuffer,
                                      .memoryLocation  = VriMemoryLocation_HostUpload};
            if (core.CreateBuffer(m_device->Handle(), &desc, &m_buffers[m_ring]) != VriResult_Success)
            {
                m_buffers[m_ring]    = nullptr;
                m_capacities[m_ring] = 0;
                return {};
            }
            m_capacities[m_ring] = cap;
        }
        if (void* mapped = core.MapBuffer(m_buffers[m_ring], 0, bytes))
        {
            std::memcpy(mapped, m_vertices.data(), bytes);
            core.UnmapBuffer(m_buffers[m_ring]);
        }
        return {m_ring, count};
    }

    void Hud::draw(fg::RenderContext& rc, Extent2D targetExtent, VriFormat colorFormat, const Batch& batch)
    {
        if (batch.count == 0 || batch.ring >= kRing || !m_buffers[batch.ring])
            return;
        VriPipeline* pipeline = pipelineFor(colorFormat);
        if (!pipeline)
            return;

        const auto& core = rc.device.Core();
        core.CmdSetPipelineLayout(rc.cmd, m_layout);
        core.CmdSetPipeline(rc.cmd, pipeline);

        const HudPush push {
            glm::vec2 {1.0f / static_cast<float>(targetExtent.width), 1.0f / static_cast<float>(targetExtent.height)},
            glm::vec2 {0.0f}};
        core.CmdSetConstants(rc.cmd, 0, &push, sizeof(push));

        const VriVertexBufferBinding vb {m_buffers[batch.ring], 0};
        core.CmdSetVertexBuffers(rc.cmd, 0, &vb, 1);
        const VriDrawDesc draw {.vertexNum = batch.count, .instanceNum = 1};
        core.CmdDraw(rc.cmd, &draw);
    }
} // namespace vrf
