/*
 * imgui_backend.hpp - Dear ImGui rendering for HOST-OWNED frame loops.
 *
 * RenderModule embeds ImGui inside its own default pass; an app that drives its
 * frames directly (framegraph, XR) uses this instead: it owns the VriImgui
 * renderer and flattens ImDrawData into VRI's neutral draw data.
 *
 * Per frame: ImGui::Render() -> Upload(GetDrawData(), extent) BEFORE recording,
 * then CmdDraw(cmd) INSIDE the final color pass (dynamic rendering open).
 * Window::InitImGui/NewImGuiFrame still provide the platform side.
 *
 * Only available with vrf_with_imgui (VRF_WITH_IMGUI).
 */
#pragma once

#if defined(VRF_WITH_IMGUI)

#include <cstdint>
#include <vector>

#include "vrf/gpu/vri_types.hpp"
#include <vri/ext/vri_ext_imgui.h>
#include <vri/vri.h>

#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"

struct ImDrawData; // Dear ImGui (global namespace)

namespace vrf
{
    class RenderDevice;

    class ImGuiBackend
    {
    public:
        ImGuiBackend() = default;
        ~ImGuiBackend();

        ImGuiBackend(const ImGuiBackend&)            = delete;
        ImGuiBackend& operator=(const ImGuiBackend&) = delete;
        ImGuiBackend(ImGuiBackend&& other) noexcept;
        ImGuiBackend& operator=(ImGuiBackend&& other) noexcept;

        // fontPixels: RGBA8 atlas from io.Fonts->GetTexDataAsRGBA32. Returns the
        // font texture view to install as ImGui's default texture id.
        [[nodiscard]] static Expected<ImGuiBackend> Create(RenderDevice&,
                                                           VriFormat   colorFormat,
                                                           VriFormat   depthFormat,
                                                           const void* fontPixels,
                                                           uint32_t    fontWidth,
                                                           uint32_t    fontHeight);

        [[nodiscard]] DescriptorHandle* FontView() const noexcept { return m_fontView; }

        // Flatten + stage this frame's geometry. Call after ImGui::Render(),
        // BEFORE the frame's command buffer is submitted-recording may overlap.
        void Upload(ImDrawData* drawData, Extent2D framebufferExtent);

        // Record the staging -> device geometry copy; OUTSIDE any render pass,
        // before CmdDraw (skipping it leaves the draw with no geometry).
        void CmdCopy(CommandBufferHandle* cmd);

        // Record the UI draws; inside an open color pass matching colorFormat.
        void CmdDraw(CommandBufferHandle* cmd);

        // Evict a texture view from the renderer's per-view descriptor-set cache. MUST be
        // called before destroying any view that was ever used as an ImTextureID: the cache
        // is keyed by pointer, so a later texture whose descriptor reuses the freed address
        // would otherwise bind the stale set (destroyed imageView -> GPU fault, and on
        // Windows a black window after aggressive resizes).
        void FreeTexture(DescriptorHandle* textureView);

    private:
        void Reset() noexcept;

        RenderDevice*     m_device = nullptr;
        VriImguiInterface m_api {};
        VriImgui*         m_gui      = nullptr;
        DescriptorHandle* m_fontView = nullptr;

        bool                             m_hasData = false;
        std::vector<VriImguiVertex>      m_vertices;
        std::vector<uint16_t>            m_indices;
        std::vector<VriImguiDrawCommand> m_commands;
        VriImguiDrawData                 m_data {};
    };
} // namespace vrf

#endif // VRF_WITH_IMGUI
