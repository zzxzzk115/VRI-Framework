/*
 * plot_view.hpp - a vplot figure rendered into a GPU texture, and optionally into an ImGui panel.
 *
 * vplot draws publication-quality figures behind a pure C ABI and can hand back either an RGBA
 * buffer or a PNG/PDF/SVG file. This wraps the first of those in the framework's texture lifetime
 * so a figure can be inspected live, and leaves the second exposed, so the figure the user just
 * resized on screen is the same object that gets written to the paper.
 *
 * The figure is owned here but deliberately NOT wrapped: plotting goes through the vpl C API
 * directly via figure(). That surface is several hundred entry points whose whole purpose is to
 * mirror matplotlib, and a hand-written facade over it would be a maintenance cost with no reader.
 *
 * Compiled only under vrf_with_vplot; without it this header defines nothing.
 */
#pragma once

#ifdef VRF_WITH_VPLOT

#include <cstdint>
#include <string>
#include <vector>

#include <vpl/vpl.h>

#include "vrf/core/result.hpp"
#include "vrf/gpu/rhi.hpp"
#include "vrf/gpu/upload.hpp"

namespace vrf
{
    class RenderDevice;

    class PlotView
    {
    public:
        // Size is in inches x dpi because that is how matplotlib - and therefore vplot - defines a
        // figure: the inch size is what a paper submission cares about, dpi only decides how many
        // pixels that becomes on screen. The defaults are matplotlib's own 6.4 x 4.8 at 100 dpi.
        [[nodiscard]] static Expected<PlotView>
        Create(RenderDevice& device, double widthInches = 6.4, double heightInches = 4.8, double dpi = 100.0);

        ~PlotView();
        PlotView(PlotView&&) noexcept;
        PlotView& operator=(PlotView&&) noexcept;
        PlotView(const PlotView&)            = delete;
        PlotView& operator=(const PlotView&) = delete;

        // The figure to plot into. Anything drawn through it needs a MarkDirty() to reach the
        // screen - vplot has no change notification, and re-rendering every frame would mean a
        // full software rasterisation plus a texture upload per frame for a static plot.
        [[nodiscard]] VplFigure* figure() const noexcept { return m_figure; }
        void                     MarkDirty() noexcept { m_dirty = true; }

        // Both preserve everything already plotted (vplFigureSetSizeInches / SetDpi re-lay-out
        // the existing artists), so a resize is not a reason to rebuild the figure.
        Expected<void> SetSizeInches(double widthInches, double heightInches);
        Expected<void> SetDpi(double dpi);

        // Re-rasterises and re-uploads if dirty; a no-op otherwise. Blocks on a staging submit,
        // so call it on a resize or a data change, not unconditionally per frame.
        Expected<void> Update(RenderDevice& device);

        [[nodiscard]] VriDescriptor* textureView() const noexcept { return m_gpu.view ? *m_gpu.view : nullptr; }
        [[nodiscard]] uint32_t       pixelWidth() const noexcept { return m_width; }
        [[nodiscard]] uint32_t       pixelHeight() const noexcept { return m_height; }

        // Writes the figure to disk. VplFormat_Auto infers from the extension. dpi applies to the
        // raster formats only - the vector ones are written at 72 dpi, one unit per point.
        Expected<void> Save(const std::string& path, VplFormat format = VplFormat_Auto, double dpi = 0.0) const;

#ifdef VRF_WITH_IMGUI
        // Draws the figure at its natural pixel size. With fitToContentRegion the figure is first
        // resized (in inches, at the current dpi) to whatever room the surrounding window gives it,
        // which is what makes a plot panel behave like every other resizable ImGui child.
        void DrawImGui(RenderDevice& device, bool fitToContentRegion = true);
#endif

    private:
        PlotView() = default;

        VplFigure*           m_figure = nullptr;
        GpuTexture           m_gpu;
        std::vector<uint8_t> m_pixels;
        uint32_t             m_width  = 0;
        uint32_t             m_height = 0;
        bool                 m_dirty  = true;
    };
} // namespace vrf

#endif // VRF_WITH_VPLOT
