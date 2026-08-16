/*
 * ui_window.hpp - dockable tool windows for ImGui apps. Pair with DockspaceHost.
 *
 * A window owns its Begin(title(), &open()) / End(). `name` is the "###id" docking and
 * imgui.ini key on, so the display label can change without losing the layout.
 * Ctx is the app's own per-frame context.
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
        explicit UIWindow(std::string name, std::string displayName = {}) :
            m_Name {std::move(name)}, m_DisplayName {displayName.empty() ? m_Name : std::move(displayName)},
            m_Title {m_DisplayName + "###" + m_Name}
        {}
        virtual ~UIWindow() = default;

        UIWindow(const UIWindow&)            = delete;
        UIWindow& operator=(const UIWindow&) = delete;

        virtual void onDraw(Ctx&) = 0;
        // Fires once on the open -> closed edge; release per-window GPU targets here.
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
