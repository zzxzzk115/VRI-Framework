/*
 * ui_window.hpp - a minimal dockable-tool-window layer for ImGui apps.
 *
 * UIWindow<Ctx> is one dockable ImGui window: a stable name (the ImGui "###id",
 * which docking / imgui.ini identity keys on), a display label that may change
 * without breaking that identity, and an open flag the host's View menu toggles.
 * UIWindowManager<Ctx> owns a set of windows and dispatches their draw each frame
 * (firing onClosed on the open -> closed edge). Ctx is the app's own per-frame
 * context type - the framework does not prescribe one.
 *
 * Header-only and ImGui-agnostic: a window wraps its own ImGui::Begin(title(), &open())
 * / End() inside onDraw. Pair with vrf::ui::DockspaceHost for the dockspace shell.
 */
#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace vrf::ui
{
    template<typename Ctx>
    class UIWindowManager;

    template<typename Ctx>
    class UIWindow
    {
    public:
        // `name` is the stable identity ("###name"); `displayName` is the visible label
        // (defaults to the name). Changing the display later keeps docking/ini intact.
        explicit UIWindow(std::string name, std::string displayName = {}) :
            m_Name {std::move(name)}, m_DisplayName {displayName.empty() ? m_Name : std::move(displayName)},
            m_Title {m_DisplayName + "###" + m_Name}
        {}
        virtual ~UIWindow() = default;

        UIWindow(const UIWindow&)            = delete;
        UIWindow& operator=(const UIWindow&) = delete;

        // Called each frame while open. Implementations wrap their body in
        // ImGui::Begin(title(), &open(), flags) / ImGui::End().
        virtual void onDraw(Ctx&) = 0;

        // Fired once on the frame the open flag flips true -> false (e.g. release
        // per-window GPU targets while hidden).
        virtual void onClosed(Ctx&) {}

        [[nodiscard]] const std::string& name() const noexcept { return m_Name; }
        [[nodiscard]] const std::string& displayName() const noexcept { return m_DisplayName; }
        // The full ImGui window title: "displayName###name".
        [[nodiscard]] const char* title() const noexcept { return m_Title.c_str(); }

        [[nodiscard]] bool& open() noexcept { return m_Open; }

    private:
        std::string m_Name;
        std::string m_DisplayName;
        std::string m_Title;
        bool        m_Open {true};
        bool        m_WasOpen {true};

        friend class UIWindowManager<Ctx>;
    };

    template<typename Ctx>
    class UIWindowManager
    {
    public:
        template<typename T, typename... Args>
        T& add(Args&&... args)
        {
            static_assert(std::is_base_of_v<UIWindow<Ctx>, T>, "T must derive from UIWindow<Ctx>");
            auto window = std::make_unique<T>(std::forward<Args>(args)...);
            T&   ref    = *window;
            m_Windows.push_back(std::move(window));
            return ref;
        }

        // Draw every open window; fire onClosed on the open -> closed edge.
        void draw(Ctx& ctx)
        {
            for (auto& window : m_Windows)
            {
                if (window->m_Open)
                {
                    window->onDraw(ctx);
                }
                else if (window->m_WasOpen)
                {
                    window->onClosed(ctx);
                }
                window->m_WasOpen = window->m_Open;
            }
        }

        // For the host's View menu (iterate: MenuItem(displayName, nullptr, &open())).
        [[nodiscard]] std::vector<std::unique_ptr<UIWindow<Ctx>>>& windows() noexcept { return m_Windows; }

    private:
        std::vector<std::unique_ptr<UIWindow<Ctx>>> m_Windows;
    };
} // namespace vrf::ui
