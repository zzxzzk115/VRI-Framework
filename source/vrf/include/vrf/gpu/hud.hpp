/*
 * hud.hpp - a tiny native in-game UI / HUD renderer (NOT ImGui).
 *
 * Immediate-mode: clear() a batch each frame, add rect()/text() in target-pixel coordinates, then
 * draw() into any OPEN color pass - the window backbuffer or one layer of a VR stereo eye target
 * (head-locked HUD). Text is solid colored quads from stb_easy_font (no font atlas/texture), so the
 * whole thing is one alpha-blended colored-triangle pipeline. The caller supplies the trivial shader
 * SPIR-V (the app already cooks Slang); vrf owns the batching, buffers, pipeline and draw.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    class RenderDevice;
    namespace fg
    {
        class RenderContext;
    }

    class Hud
    {
    public:
        struct Shader
        {
            const void* spirv {nullptr};
            size_t      size {0};
            const char* entry {"main"};
        };

        // colorFormat must match the target pass (backbuffer / eye) the HUD draws into.
        [[nodiscard]] static Expected<Hud> Create(RenderDevice&, VriFormat colorFormat, Shader vertex, Shader fragment);

        Hud() = default;
        ~Hud();
        Hud(const Hud&)            = delete;
        Hud& operator=(const Hud&) = delete;
        Hud(Hud&&) noexcept;
        Hud& operator=(Hud&&) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept { return m_device != nullptr; }

        // ---- batch (coordinates in target pixels, origin top-left) ----
        void clear() noexcept { m_vertices.clear(); }
        void rect(float x, float y, float w, float h, const glm::vec4& color);
        void text(const std::string& str, float x, float y, float scale, const glm::vec4& color);
        [[nodiscard]] float textWidth(const std::string& str, float scale) const;
        [[nodiscard]] float textHeight(float scale) const;

        // Stage this frame's batch into a host-visible vertex buffer (cycles frames-in-flight
        // internally). Call once per frame, before recording; then draw() as many times as needed.
        void upload();

        // Record the HUD draws into an OPEN color pass of `targetExtent` (the caller opened the
        // framebuffer: backbuffer, or an eye layer via AttachmentInfo::layer).
        void draw(fg::RenderContext&, Extent2D targetExtent);

    private:
        void reset() noexcept;

        struct Vertex
        {
            glm::vec2 pos; // target pixels
            glm::vec4 col;
        };

        RenderDevice*       m_device {nullptr};
        VriPipeline*        m_pipeline {nullptr};
        VriPipelineLayout*  m_layout {nullptr};
        std::vector<Vertex> m_vertices;

        static constexpr uint32_t kFramesInFlight = 2;
        VriBuffer*                m_buffers[kFramesInFlight] {};
        size_t                    m_capacities[kFramesInFlight] {};
        uint32_t                  m_frame {0};
        uint32_t                  m_drawCount {0}; // vertices in the current frame's buffer
    };
} // namespace vrf
