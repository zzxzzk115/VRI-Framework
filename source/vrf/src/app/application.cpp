#include "vrf/app/application.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#if defined(VRF_WITH_IMGUI)
#include <imgui.h>
#endif

namespace vrf
{
    namespace
    {
        // Backend selection follows VRI's own convention (examples/common/example_app.h): the
        // VRI_API env var (vulkan|d3d12|opengl|metal|webgpu) forces a backend, and anything else
        // - including unset - leaves it to VriGraphicsAPI_Auto. Reusing VRI's env-var name (rather
        // than a parallel one) keeps the framework consistent with VRI for anyone who knows it.
        // Only consulted when the caller left desc.api as Auto, so explicit code always wins.
        GraphicsApi ResolveApi(GraphicsApi requested)
        {
            if (requested != GraphicsApi::Auto)
                return requested;
            const char* env = std::getenv("VRI_API");
            if (env == nullptr)
                return GraphicsApi::Auto;
            if (std::strcmp(env, "vulkan") == 0)
                return GraphicsApi::Vulkan;
            if (std::strcmp(env, "d3d12") == 0 || std::strcmp(env, "dx12") == 0)
                return GraphicsApi::D3D12;
            if (std::strcmp(env, "opengl") == 0 || std::strcmp(env, "gl") == 0)
                return GraphicsApi::OpenGL;
            if (std::strcmp(env, "metal") == 0)
                return GraphicsApi::Metal;
            if (std::strcmp(env, "webgpu") == 0 || std::strcmp(env, "wgpu") == 0)
                return GraphicsApi::WebGPU;
            return GraphicsApi::Auto;
        }
    } // namespace

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
        moduleDesc.api             = ResolveApi(desc.api);
        moduleDesc.enabledFeatures = desc.enabledFeatures;
        moduleDesc.validation      = desc.validation;
        moduleDesc.enableImGui     = desc.imgui;
        moduleDesc.window          = (*window)->Handle();
        moduleDesc.nativeDisplay   = (*window)->NativeDisplay();
        moduleDesc.extent          = (*window)->Extent();
        moduleDesc.colorFormat     = desc.colorFormat;
        moduleDesc.depthFormat     = desc.depthFormat;
        moduleDesc.presentMode     = desc.presentMode;
        moduleDesc.clearColor[0]   = desc.clearColor[0];
        moduleDesc.clearColor[1]   = desc.clearColor[1];
        moduleDesc.clearColor[2]   = desc.clearColor[2];
        moduleDesc.clearColor[3]   = desc.clearColor[3];
        moduleDesc.clearDepth      = desc.clearDepth;

        auto module = RenderModule::Create(moduleDesc);
        if (!module)
            return std::unexpected(module.error());

        auto app      = std::unique_ptr<Application>(new Application());
        app->m_desc   = desc;
        app->m_window = std::move(*window);
        app->m_module = std::move(*module);

        // Reflect the resolved backend (+ API version) in the title bar, e.g. "vrf (Vulkan 1.4)".
        {
            const RenderDevice& device = app->m_module->Device();
            std::string         title(desc.title);
            title += " (";
            title += device.ApiName();
            if (const VriDeviceDesc* dd = device.Desc();
                dd != nullptr && (dd->apiVersionMajor != 0 || dd->apiVersionMinor != 0))
                title += " " + std::to_string(dd->apiVersionMajor) + "." + std::to_string(dd->apiVersionMinor);
            title += ")";
            app->m_window->SetTitle(title.c_str());
        }

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
