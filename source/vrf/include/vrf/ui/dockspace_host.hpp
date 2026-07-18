/*
 * dockspace_host.hpp - the full-viewport dockspace shell for UIWindow-based tools.
 *
 * begin() opens a borderless host window pinned to the main viewport and submits the
 * dockspace; when the dock node does not exist yet (first run with no imgui.ini, or
 * after resetLayout()) the caller's buildLayout callback runs inside a DockBuilder
 * transaction to lay out the default splits. end() closes the host.
 *
 *   host.begin(desc, [](ImGuiID dockId, ImVec2 workSize) {
 *       ImGuiID center = dockId;
 *       ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
 *       ImGui::DockBuilderDockWindow("Viewport###Viewport", center);
 *       ImGui::DockBuilderDockWindow("Controls###Controls", right);
 *   });
 *   ... draw menu bar (desc.menuBar) + windows ...
 *   host.end();
 *
 * The dockspace id is a fixed hash of the host name (not the id stack), so layout
 * identity is stable across frames and code moves. Windows are docked by their full
 * "display###name" title (see UIWindow::title()).
 */
#pragma once

#if defined(VRF_WITH_IMGUI)

#include <functional>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder* + ImHashStr

namespace vrf::ui
{
    class DockspaceHost
    {
    public:
        struct Desc
        {
            const char* name = "DockSpaceHost";
            // Adds a menu bar region to the host; draw it with ImGui::BeginMenuBar between
            // begin() and end().
            bool menuBar = false;
            // Transparent central node: an empty central dock node lets the app's own
            // backbuffer rendering show through (host gets NoBackground + PassthruCentralNode).
            bool passthruCentralNode = false;
            // Minimum docked-panel width while the dockspace is submitted (0 = ImGui default).
            float minPanelWidth = 0.0f;
            // Extra dockspace flags OR-ed in (e.g. ImGuiDockNodeFlags_NoWindowMenuButton).
            ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_None;
        };

        // buildLayout(dockId, workSize): split `dockId` and DockBuilderDockWindow the app's
        // windows. Add/SetNodeSize/Finish are handled here; only the splits + docking go in.
        using BuildLayout = std::function<void(ImGuiID, ImVec2)>;

        void begin(const Desc& desc, const BuildLayout& buildLayout)
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2 {0.0f, 0.0f});
            ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoNavFocus;
            if (desc.menuBar)
                hostFlags |= ImGuiWindowFlags_MenuBar;
            if (desc.passthruCentralNode)
                hostFlags |= ImGuiWindowFlags_NoBackground;
            ImGui::Begin(desc.name, nullptr, hostFlags);
            ImGui::PopStyleVar(3);

            const ImGuiID dockId = ImHashStr(desc.name);
            if (m_ResetRequested)
            {
                ImGui::DockBuilderRemoveNode(dockId);
                m_ResetRequested = false;
            }
            if (ImGui::DockBuilderGetNode(dockId) == nullptr)
            {
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, viewport->WorkSize);
                if (buildLayout)
                {
                    buildLayout(dockId, viewport->WorkSize);
                }
                ImGui::DockBuilderFinish(dockId);
            }

            ImGuiStyle& style       = ImGui::GetStyle();
            const float minWinSizeX = style.WindowMinSize.x;
            if (desc.minPanelWidth > 0.0f)
                style.WindowMinSize.x = desc.minPanelWidth;
            ImGuiDockNodeFlags dockFlags = desc.dockFlags;
            if (desc.passthruCentralNode)
                dockFlags |= ImGuiDockNodeFlags_PassthruCentralNode;
            ImGui::DockSpace(dockId, ImVec2 {0.0f, 0.0f}, dockFlags);
            style.WindowMinSize.x = minWinSizeX;
        }

        void end() { ImGui::End(); }

        // Rebuild the default layout on the next begin() (a "Reset Layout" menu action).
        void resetLayout() { m_ResetRequested = true; }

    private:
        bool m_ResetRequested {false};
    };
} // namespace vrf::ui

#endif // VRF_WITH_IMGUI
