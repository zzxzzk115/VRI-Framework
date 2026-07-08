#include "vrf/app/application.hpp"

#include <cstdint>
#include <cstdlib>
#include <utility>

#if defined(VRF_WITH_IMGUI)
#include <imgui.h>
#endif

namespace vrf
{
    Expected<std::unique_ptr<Application>> Application::Create(const ApplicationDesc& desc)
    {
        WindowDesc windowDesc;
        windowDesc.title   = desc.title;
        windowDesc.extent  = desc.extent;
        windowDesc.backend = desc.windowBackend;

        auto window = Window::Create(windowDesc);
        if (!window)
            return std::unexpected(window.error());

        RenderModuleDesc moduleDesc;
        moduleDesc.api           = desc.api;
        moduleDesc.validation    = desc.validation;
        moduleDesc.enableImGui   = desc.imgui;
        moduleDesc.window        = (*window)->Handle();
        moduleDesc.nativeDisplay = (*window)->NativeDisplay();
        moduleDesc.extent        = (*window)->Extent();
        moduleDesc.colorFormat   = desc.colorFormat;
        moduleDesc.depthFormat   = desc.depthFormat;
        moduleDesc.presentMode   = desc.presentMode;
        moduleDesc.clearColor[0] = desc.clearColor[0];
        moduleDesc.clearColor[1] = desc.clearColor[1];
        moduleDesc.clearColor[2] = desc.clearColor[2];
        moduleDesc.clearColor[3] = desc.clearColor[3];
        moduleDesc.clearDepth    = desc.clearDepth;

        auto module = RenderModule::Create(moduleDesc);
        if (!module)
            return std::unexpected(module.error());

        auto app      = std::unique_ptr<Application>(new Application());
        app->m_desc   = desc;
        app->m_window = std::move(*window);
        app->m_module = std::move(*module);

#if defined(VRF_WITH_IMGUI)
        if (desc.imgui)
        {
            auto imguiResult = app->InitImGui();
            if (!imguiResult)
                return std::unexpected(imguiResult.error());
        }
#endif
        return app;
    }

#if defined(VRF_WITH_IMGUI)
    Expected<void> Application::InitImGui()
    {
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiIO& io    = ImGui::GetIO();
        io.IniFilename = nullptr; // don't write imgui.ini next to the app

        if (!m_window->InitImGui())
            return MakeError("Application::InitImGui: window backend ImGui init failed");

        unsigned char* pixels = nullptr;
        int            width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        auto fontView = m_module->InitImGui(pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        if (!fontView)
            return std::unexpected(fontView.error());
        io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(*fontView));

        m_imguiActive = true;
        return {};
    }
#endif

    Application::~Application()
    {
#if defined(VRF_WITH_IMGUI)
        if (m_imguiActive)
        {
            if (m_module)
                m_module->Device().WaitIdle();
            m_window->ShutdownImGui();
            ImGui::DestroyContext();
        }
#endif
    }

    void Application::SetClearColor(float r, float g, float b, float a)
    {
        m_desc.clearColor[0] = r;
        m_desc.clearColor[1] = g;
        m_desc.clearColor[2] = b;
        m_desc.clearColor[3] = a;
        if (m_module)
            m_module->SetClearColor(r, g, b, a);
    }

    void Application::Run()
    {
        // Optional headless auto-exit: VRF_MAX_FRAMES=N presents N frames then returns.
        const char*    maxFramesEnv = std::getenv("VRF_MAX_FRAMES");
        const uint64_t maxFrames    = maxFramesEnv ? std::strtoull(maxFramesEnv, nullptr, 10) : 0;
        uint64_t       frames       = 0;

        while (!m_window->ShouldClose())
        {
            m_window->PollEvents();
            if (m_window->ShouldClose())
                break;

#if defined(VRF_WITH_IMGUI)
            if (m_imguiActive)
            {
                m_window->NewImGuiFrame(); // platform new frame (sets io.DisplaySize + input)
                ImGui::NewFrame();
                if (onGui)
                    onGui();
                ImGui::Render();
                m_module->UploadImGui(ImGui::GetDrawData()); // stage geometry before acquire
            }
#endif

            FrameContext frame = m_module->BeginFrame();
            if (!frame.valid)
            {
                m_module->Resize(m_window->Extent());
                continue;
            }
            if (onRecord)
                onRecord(frame.cmd);
            m_module->EndFrame();

            ++frames;
            if (maxFrames != 0 && frames >= maxFrames)
                break;
        }
        m_module->Device().WaitIdle();
    }
} // namespace vrf
