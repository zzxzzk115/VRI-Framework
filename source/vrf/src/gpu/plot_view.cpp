#include "vrf/gpu/plot_view.hpp"

#ifdef VRF_WITH_VPLOT

#include <algorithm>
#include <cmath>
#include <utility>

#include "vrf/asset/texture.hpp"
#include "vrf/core/log.hpp"
#include "vrf/gpu/render_device.hpp"

#ifdef VRF_WITH_IMGUI
#include <imgui.h>
#endif

namespace vrf
{
    namespace
    {
        // vplot's failure detail is per thread and only valid until the next vpl call on it.
        [[nodiscard]] Error PlotError(const VplResult result, const char* what)
        {
            return Error {VriResult_Failure,
                          std::string {what} + ": " + vplResultToString(result) + " (" + vplGetLastError() + ")"};
        }
    } // namespace

    void PlotView::SetFontPath(const std::string& ttfPath)
    {
        if (const auto r = vplSetFontPath(ttfPath.c_str()); r != VplResult_Success)
        {
            LogWarning("plot view: font {} rejected: {}", ttfPath, vplResultToString(r));
        }
    }

    Expected<PlotView> PlotView::Create(const double widthInches, const double heightInches, const double dpi)
    {

        VplFigureDesc desc {};
        desc.structSize   = sizeof(VplFigureDesc);
        desc.widthInches  = widthInches;
        desc.heightInches = heightInches;
        desc.dpi          = dpi;

        VplFigure* figure = nullptr;
        if (const auto r = vplCreateFigure(&desc, &figure); r != VplResult_Success)
        {
            return std::unexpected(PlotError(r, "vplCreateFigure"));
        }

        PlotView view;
        view.m_figure = figure;
        // Left dirty and untextured: DrawImGui() calls Update() before it reaches ImGui::Image,
        // and a save-only caller never needs the texture.
        return view;
    }

    PlotView::~PlotView()
    {
        if (m_figure != nullptr)
        {
            vplDestroyFigure(m_figure);
        }
    }

    PlotView::PlotView(PlotView&& other) noexcept :
        m_figure {std::exchange(other.m_figure, nullptr)}, m_gpu {std::move(other.m_gpu)},
        m_pixels {std::move(other.m_pixels)}, m_width {other.m_width}, m_height {other.m_height},
        m_dirty {other.m_dirty}
    {}

    PlotView& PlotView::operator=(PlotView&& other) noexcept
    {
        if (this != &other)
        {
            if (m_figure != nullptr)
            {
                vplDestroyFigure(m_figure);
            }
            m_figure = std::exchange(other.m_figure, nullptr);
            m_gpu    = std::move(other.m_gpu);
            m_pixels = std::move(other.m_pixels);
            m_width  = other.m_width;
            m_height = other.m_height;
            m_dirty  = other.m_dirty;
        }
        return *this;
    }

    Expected<void> PlotView::SetSizeInches(const double widthInches, const double heightInches)
    {
        if (const auto r = vplFigureSetSizeInches(m_figure, widthInches, heightInches); r != VplResult_Success)
        {
            return std::unexpected(PlotError(r, "vplFigureSetSizeInches"));
        }
        m_dirty = true;
        return {};
    }

    Expected<void> PlotView::SetDpi(const double dpi)
    {
        if (const auto r = vplFigureSetDpi(m_figure, dpi); r != VplResult_Success)
        {
            return std::unexpected(PlotError(r, "vplFigureSetDpi"));
        }
        m_dirty = true;
        return {};
    }

    Expected<void> PlotView::Update(RenderDevice& device)
    {
        if (!m_dirty)
        {
            return {};
        }

        uint32_t width = 0, height = 0;
        if (const auto r = vplFigureGetPixelSize(m_figure, &width, &height); r != VplResult_Success)
        {
            return std::unexpected(PlotError(r, "vplFigureGetPixelSize"));
        }
        if (width == 0 || height == 0)
        {
            return std::unexpected(Error {VriResult_InvalidArgument, "plot view: figure has zero pixel size"});
        }

        // Query the size rather than computing width * height * 4: a null buffer asks for it.
        size_t required = 0;
        if (const auto r = vplFigureRenderRGBA(m_figure, nullptr, 0, &required);
            r != VplResult_Success && r != VplResult_BufferTooSmall)
        {
            return std::unexpected(PlotError(r, "vplFigureRenderRGBA (size query)"));
        }
        m_pixels.resize(required);
        if (const auto r = vplFigureRenderRGBA(m_figure, m_pixels.data(), m_pixels.size(), nullptr);
            r != VplResult_Success)
        {
            return std::unexpected(PlotError(r, "vplFigureRenderRGBA"));
        }

        // Row 0 is the top row on vplot's side, which is what a sampled 2D texture expects.
        Texture cpu;
        cpu.name   = "vplot figure";
        cpu.width  = width;
        cpu.height = height;
        cpu.format = VriFormat_RGBA8_UNORM;
        cpu.data.assign(m_pixels.begin(), m_pixels.end());

        auto uploaded = UploadTexture(device, cpu);
        if (!uploaded)
        {
            return std::unexpected(uploaded.error());
        }

        // Released only now: a figure that fails to re-render keeps showing the last good one.
        m_gpu    = std::move(uploaded.value());
        m_width  = width;
        m_height = height;
        m_dirty  = false;
        return {};
    }

    Expected<void> PlotView::Save(const std::string& path, const VplFormat format, const double dpi) const
    {
        VplSaveDesc desc {};
        desc.structSize = sizeof(VplSaveDesc);
        desc.format     = format;
        desc.dpi        = dpi;

        // Zero dpi with an inferred format is the "infer everything from the path" case the C ABI
        // spells as desc = NULL.
        const VplSaveDesc* descPtr = (format == VplFormat_Auto && dpi == 0.0) ? nullptr : &desc;
        if (const auto r = vplFigureSaveFig(m_figure, path.c_str(), descPtr); r != VplResult_Success)
        {
            return std::unexpected(PlotError(r, "vplFigureSaveFig"));
        }
        return {};
    }

#ifdef VRF_WITH_IMGUI
    void PlotView::DrawImGui(RenderDevice& device, const bool fitToContentRegion)
    {
        if (fitToContentRegion)
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            double       dpi   = 0.0;
            if (vplFigureGetDpi(m_figure, &dpi) == VplResult_Success && dpi > 0.0)
            {
                // Whole pixels before converting back to inches: a sub-pixel wobble resizes the
                // figure every frame, and every resize re-rasterises and re-uploads.
                const auto targetW = static_cast<uint32_t>(std::max(1.0f, std::floor(avail.x)));
                const auto targetH = static_cast<uint32_t>(std::max(1.0f, std::floor(avail.y)));
                if (targetW != m_width || targetH != m_height)
                {
                    if (auto resized = SetSizeInches(targetW / dpi, targetH / dpi); !resized)
                    {
                        LogWarning("plot view: resize failed: {}", resized.error().message);
                    }
                }
            }
        }

        if (auto updated = Update(device); !updated)
        {
            LogWarning("plot view: update failed: {}", updated.error().message);
        }

        VriDescriptor* view = textureView();
        if (view == nullptr)
        {
            return;
        }
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(view)),
                     ImVec2 {static_cast<float>(m_width), static_cast<float>(m_height)});
    }
#endif
} // namespace vrf

#endif // VRF_WITH_VPLOT
