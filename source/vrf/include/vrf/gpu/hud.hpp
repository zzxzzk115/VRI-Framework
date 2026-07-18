/*
 * hud.hpp - a tiny native in-game UI / HUD renderer (NOT ImGui).
 *
 * Immediate-mode, one batch at a time: clear(), add rect()/text() in target-pixel coordinates,
 * upload(), then draw() into any OPEN color pass - the window backbuffer or one layer of a VR
 * stereo eye target (head-locked HUD). Repeat clear/upload/draw for each distinct batch/target in a
 * frame (a small ring of vertex buffers keeps in-flight batches independent). Text is solid colored
 * quads from stb_easy_font (no font atlas/texture), so it's one alpha-blended colored-triangle
 * pipeline, lazily built per color format. The caller supplies the trivial shader SPIR-V.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

        [[nodiscard]] static Expected<Hud> Create(RenderDevice&, Shader vertex, Shader fragment);

        Hud() = default;
        ~Hud();
        Hud(const Hud&)            = delete;
        Hud& operator=(const Hud&) = delete;
        Hud(Hud&&) noexcept;
        Hud& operator=(Hud&&) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept { return m_device != nullptr; }

        // ---- batch (coordinates in target pixels, origin top-left) ----
        void                clear() noexcept { m_vertices.clear(); }
        void                rect(float x, float y, float w, float h, const glm::vec4& color);
        void                text(const std::string& str, float x, float y, float scale, const glm::vec4& color);
        [[nodiscard]] float textWidth(const std::string& str, float scale) const;
        [[nodiscard]] float textHeight(float scale) const;

        // A handle to one uploaded batch (so several batches can coexist in a frame - e.g. the
        // desktop overlay plus a per-eye HUD - each drawn independently).
        struct Batch
        {
            uint32_t ring {0};
            uint32_t count {0};
        };

        // Stage the current batch into a fresh ring vertex buffer; returns its handle.
        [[nodiscard]] Batch upload();

        // Record `batch` into an OPEN color pass of `targetExtent` and `colorFormat` (pipelines are
        // cached per format).
        void draw(fg::RenderContext&, Extent2D targetExtent, VriFormat colorFormat, const Batch& batch);

    private:
        void                       reset() noexcept;
        [[nodiscard]] VriPipeline* pipelineFor(VriFormat colorFormat);

        struct Vertex
        {
            glm::vec2 pos; // target pixels
            glm::vec4 col;
        };

        RenderDevice*         m_device {nullptr};
        VriPipelineLayout*    m_layout {nullptr};
        std::vector<uint32_t> m_vsSpirv, m_fsSpirv; // owned copies (caller's may not outlive us)
        std::string           m_vsEntry {"main"}, m_fsEntry {"main"};

        struct FormatPipeline
        {
            VriFormat    format;
            VriPipeline* pipeline;
        };
        std::vector<FormatPipeline> m_pipelines;

        std::vector<Vertex> m_vertices;

        static constexpr uint32_t kRing = 8; // > batches-per-frame * frames-in-flight
        VriBuffer*                m_buffers[kRing] {};
        size_t                    m_capacities[kRing] {};
        uint32_t                  m_ring {0}; // allocation cursor
    };
} // namespace vrf
