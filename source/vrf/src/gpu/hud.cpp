#include "vrf/gpu/hud.hpp"

#include <cstring>
#include <string>
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

        // stb_easy_font packs a fixed ~6x7 cell; scale 1 => ~7px tall. `scale` multiplies that.
        constexpr float kStbFontHeight = 7.0f;
    } // namespace

    Expected<Hud> Hud::Create(RenderDevice& device, VriFormat colorFormat, Shader vertex, Shader fragment)
    {
        // Layout: just a push constant (no descriptor sets - text is untextured colored triangles).
        PipelineLayoutBuilder layoutBuilder;
        layoutBuilder.SetShaderStages(VriShaderStage_AllGraphics);
        layoutBuilder.AddPushConstant(0, sizeof(HudPush), VriShaderStage_Vertex);
        auto layout = layoutBuilder.Build(device);
        if (!layout)
            return std::unexpected(layout.error());

        VriBlendDesc blend {};
        blend.enable   = VRI_TRUE;
        blend.srcColor = VriBlendFactor_SrcAlpha;
        blend.dstColor = VriBlendFactor_OneMinusSrcAlpha;
        blend.colorOp  = VriBlendOp_Add;
        blend.srcAlpha = VriBlendFactor_One;
        blend.dstAlpha = VriBlendFactor_OneMinusSrcAlpha;
        blend.alphaOp  = VriBlendOp_Add;

        auto pipeline = GraphicsPipelineBuilder {}
                            .SetPipelineLayout(*layout)
                            .AddShader(VriShaderStage_Vertex, vertex.spirv, vertex.size, vertex.entry)
                            .AddShader(VriShaderStage_Fragment, fragment.spirv, fragment.size, fragment.entry)
                            .SetTopology(VriPrimitiveTopology_TriangleList)
                            .SetCullMode(VriCullMode_None)
                            .AddVertexStream(sizeof(Vertex), 0)
                            .AddVertexAttribute(VriFormat_RG32_SFLOAT, offsetof(Vertex, pos), 0)   // POSITION
                            .AddVertexAttribute(VriFormat_RGBA32_SFLOAT, offsetof(Vertex, col), 0) // COLOR0
                            .AddColorAttachment(colorFormat, VriColorWrite_RGBA, blend)
                            .Build(device);
        if (!pipeline)
            return std::unexpected(pipeline.error());

        Hud hud;
        hud.m_device   = &device;
        hud.m_layout   = *layout;
        hud.m_pipeline = *pipeline;
        return hud;
    }

    Hud::Hud(Hud&& o) noexcept { *this = std::move(o); }

    Hud& Hud::operator=(Hud&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            m_device   = o.m_device;
            m_pipeline = o.m_pipeline;
            m_layout   = o.m_layout;
            m_vertices = std::move(o.m_vertices);
            m_frame    = o.m_frame;
            m_drawCount = o.m_drawCount;
            for (uint32_t i = 0; i < kFramesInFlight; ++i)
            {
                m_buffers[i]    = o.m_buffers[i];
                m_capacities[i] = o.m_capacities[i];
            }
            o.m_device   = nullptr;
            o.m_pipeline = nullptr;
            o.m_layout   = nullptr;
            for (uint32_t i = 0; i < kFramesInFlight; ++i)
            {
                o.m_buffers[i]    = nullptr;
                o.m_capacities[i] = 0;
            }
        }
        return *this;
    }

    Hud::~Hud() { reset(); }

    void Hud::reset() noexcept
    {
        if (!m_device)
            return;
        const auto& core = m_device->Core();
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
            if (m_buffers[i])
                core.DestroyBuffer(m_buffers[i]);
        if (m_pipeline)
            core.DestroyPipeline(m_pipeline);
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
        // stb_easy_font emits quads of {float x,y,z; uchar rgba[4]}; we take pos, scale, recolor.
        static thread_local std::vector<char> buffer;
        buffer.resize(str.size() * 270 + 512); // ~270 bytes/char worst case (4 verts * 16 + slack)
        const int quads =
            stb_easy_font_print(0.0f, 0.0f, const_cast<char*>(str.c_str()), nullptr, buffer.data(),
                                static_cast<int>(buffer.size()));
        struct StbVert
        {
            float         x, y, z;
            unsigned char color[4];
        };
        const auto* verts = reinterpret_cast<const StbVert*>(buffer.data());
        for (int q = 0; q < quads; ++q)
        {
            const StbVert& v0 = verts[q * 4 + 0];
            const StbVert& v1 = verts[q * 4 + 1];
            const StbVert& v2 = verts[q * 4 + 2];
            const StbVert& v3 = verts[q * 4 + 3];
            auto           p  = [&](const StbVert& v) {
                return glm::vec2 {x + v.x * scale, y + v.y * scale};
            };
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

    void Hud::upload()
    {
        if (!m_device)
            return;
        m_frame     = (m_frame + 1) % kFramesInFlight;
        m_drawCount = static_cast<uint32_t>(m_vertices.size());
        if (m_drawCount == 0)
            return;

        const auto&    core  = m_device->Core();
        const uint64_t bytes = m_vertices.size() * sizeof(Vertex);
        if (m_capacities[m_frame] < bytes)
        {
            if (m_buffers[m_frame])
                core.DestroyBuffer(m_buffers[m_frame]);
            const uint64_t     cap = bytes + bytes / 2 + 4096; // grow with slack
            const VriBufferDesc desc {.size           = cap,
                                      .structureStride = 0,
                                      .usage          = VriBufferUsage_VertexBuffer,
                                      .memoryLocation = VriMemoryLocation_HostUpload};
            if (core.CreateBuffer(m_device->Handle(), &desc, &m_buffers[m_frame]) != VriResult_Success)
            {
                m_buffers[m_frame]    = nullptr;
                m_capacities[m_frame] = 0;
                m_drawCount           = 0;
                return;
            }
            m_capacities[m_frame] = cap;
        }
        if (void* mapped = core.MapBuffer(m_buffers[m_frame], 0, bytes))
        {
            std::memcpy(mapped, m_vertices.data(), bytes);
            core.UnmapBuffer(m_buffers[m_frame]);
        }
    }

    void Hud::draw(fg::RenderContext& rc, Extent2D targetExtent)
    {
        if (m_drawCount == 0 || !m_buffers[m_frame])
            return;
        const auto& core = rc.device.Core();
        core.CmdSetPipelineLayout(rc.cmd, m_layout);
        core.CmdSetPipeline(rc.cmd, m_pipeline);

        const HudPush push {glm::vec2 {1.0f / static_cast<float>(targetExtent.width),
                                       1.0f / static_cast<float>(targetExtent.height)},
                            glm::vec2 {0.0f}};
        core.CmdSetConstants(rc.cmd, 0, &push, sizeof(push));

        const VriVertexBufferBinding vb {m_buffers[m_frame], 0};
        core.CmdSetVertexBuffers(rc.cmd, 0, &vb, 1);
        const VriDrawDesc draw {.vertexNum = m_drawCount, .instanceNum = 1};
        core.CmdDraw(rc.cmd, &draw);
    }
} // namespace vrf
