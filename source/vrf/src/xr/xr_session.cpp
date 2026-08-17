#if defined(VRF_WITH_OPENXR)

#define XR_USE_GRAPHICS_API_VULKAN
// clang-format off
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// clang-format on

#include "vrf/xr/xr_session.hpp"

#include <vector>

#include <vri/ext/vri_ext_interop.h>

#include "vrf/core/log.hpp"
#include "vrf/gpu/render_device.hpp"

namespace vrf::xr
{
    namespace
    {
        [[nodiscard]] VriFormat toVriFormat(const int64_t vk)
        {
            switch (vk)
            {
                case VK_FORMAT_R8G8B8A8_SRGB:
                    return VriFormat_RGBA8_SRGB;
                case VK_FORMAT_R8G8B8A8_UNORM:
                    return VriFormat_RGBA8_UNORM;
                case VK_FORMAT_B8G8R8A8_SRGB:
                    return VriFormat_BGRA8_SRGB;
                case VK_FORMAT_B8G8R8A8_UNORM:
                    return VriFormat_BGRA8_UNORM;
                default:
                    return VriFormat_Unknown;
            }
        }

        [[nodiscard]] Pose fromXr(const XrPosef& p)
        {
            return Pose {
                .orientation = Quat {p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z},
                .position    = Vec3 {p.position.x, p.position.y, p.position.z},
            };
        }

        [[nodiscard]] Fov fromXr(const XrFovf& f) { return Fov {f.angleLeft, f.angleRight, f.angleUp, f.angleDown}; }
    } // namespace

    struct XrSession::Impl
    {
        RenderDevice* device {nullptr};

        ::XrInstance  instance {XR_NULL_HANDLE};
        ::XrSession   session {XR_NULL_HANDLE};
        ::XrSpace     appSpace {XR_NULL_HANDLE};
        ::XrSwapchain swapchain {XR_NULL_HANDLE};

        Extent2D  eyeExtent {};
        VriFormat colorFormat {VriFormat_Unknown};

        std::vector<VriTexture*> wrappedImages; // owned VriTexture wrappers around XR VkImages
        std::vector<fg::Texture> targets;       // state-tracked fg wrappers over wrappedImages

        SessionState state {SessionState::Idle};
        bool         sessionBegun {false};

        // Between BeginFrame and EndFrame:
        XrFrameState frameState {XR_TYPE_FRAME_STATE};
        bool         frameBegun {false};
        uint32_t     acquiredIndex {0};
        bool         imageAcquired {false};
        XrView       locatedViews[2] {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

        float nearZ {0.05f};
        float farZ {100.0f};

        ~Impl()
        {
            targets.clear(); // destroy views before the wrapped textures
            if (device)
            {
                for (auto* wrapped : wrappedImages)
                {
                    device->Core().DestroyTexture(wrapped); // borrowed image: frees the wrapper only
                }
            }
            if (swapchain != XR_NULL_HANDLE)
                xrDestroySwapchain(swapchain);
            if (appSpace != XR_NULL_HANDLE)
                xrDestroySpace(appSpace);
            if (session != XR_NULL_HANDLE)
                xrDestroySession(session);
        }

        // A runtime that has gone away must end up in Exiting, or nothing ever tears the session
        // down: xrPollEvent just stops returning XR_SUCCESS, the state stays Running, and the app
        // renders into a null target forever instead of dropping back to the desktop rig.
        void Fail(XrResult r, const char* what)
        {
            if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST || r == XR_ERROR_RUNTIME_FAILURE)
            {
                LogWarning("vrf::xr: {} returned {} - ending the session", what, static_cast<int>(r));
                state        = SessionState::Exiting;
                sessionBegun = false;
            }
        }

        void PollEvents()
        {
            XrEventDataBuffer event {XR_TYPE_EVENT_DATA_BUFFER};
            XrResult          pr = XR_SUCCESS;
            while ((pr = xrPollEvent(instance, &event)) == XR_SUCCESS)
            {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                {
                    const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                    switch (changed->state)
                    {
                        case XR_SESSION_STATE_READY: {
                            XrSessionBeginInfo bi {XR_TYPE_SESSION_BEGIN_INFO};
                            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                            if (XR_SUCCEEDED(xrBeginSession(session, &bi)))
                            {
                                sessionBegun = true;
                                state        = SessionState::Running;
                            }
                            break;
                        }
                        case XR_SESSION_STATE_STOPPING:
                            xrEndSession(session);
                            sessionBegun = false;
                            state        = SessionState::Stopping;
                            break;
                        case XR_SESSION_STATE_EXITING:
                        case XR_SESSION_STATE_LOSS_PENDING:
                            state = SessionState::Exiting;
                            break;
                        default:
                            break;
                    }
                }
                else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
                {
                    state = SessionState::Exiting;
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }
            // XR_EVENT_UNAVAILABLE is the normal end of the queue; anything else is the runtime.
            if (pr != XR_EVENT_UNAVAILABLE)
            {
                Fail(pr, "xrPollEvent");
            }
        }
    };

    XrSession::XrSession(std::unique_ptr<Impl> impl) noexcept : m_impl {std::move(impl)} {}
    XrSession::~XrSession() = default;

    Expected<std::unique_ptr<XrSession>> XrSession::Create(XrSystem& system, RenderDevice& device)
    {
        auto impl      = std::make_unique<Impl>();
        impl->device   = &device;
        impl->instance = system.Instance();

        // Native Vulkan handles -> XrGraphicsBindingVulkanKHR. Only meaningful
        // when the device was created through the system's create hooks.
        VriInteropInterface interop {};
        if (!Succeeded(vriGetInterface(device.Handle(), VRI_INTERFACE_INTEROP, sizeof(interop), &interop)))
        {
            return MakeError("xr::XrSession: device has no interop interface");
        }
        VriDeviceNativeHandles native {};
        if (!Succeeded(interop.GetDeviceNativeHandles(device.Handle(), &native)))
        {
            return MakeError("xr::XrSession: GetDeviceNativeHandles failed");
        }

        XrGraphicsBindingVulkanKHR binding {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance         = static_cast<VkInstance>(native.u.vulkan.instance);
        binding.physicalDevice   = static_cast<VkPhysicalDevice>(native.u.vulkan.physicalDevice);
        binding.device           = static_cast<VkDevice>(native.u.vulkan.device);
        binding.queueFamilyIndex = native.u.vulkan.graphicsQueueFamilyIndex;
        binding.queueIndex       = native.u.vulkan.graphicsQueueIndex;

        {
            XrSessionCreateInfo ci {XR_TYPE_SESSION_CREATE_INFO};
            ci.next     = &binding;
            ci.systemId = system.SystemId();
            if (XR_FAILED(xrCreateSession(system.Instance(), &ci, &impl->session)))
            {
                return MakeError("xr::XrSession: xrCreateSession failed (device created without XR hooks?)");
            }
        }

        {
            XrReferenceSpaceCreateInfo ci {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            ci.referenceSpaceType                 = XR_REFERENCE_SPACE_TYPE_LOCAL;
            ci.poseInReferenceSpace.orientation.w = 1.0f;
            if (XR_FAILED(xrCreateReferenceSpace(impl->session, &ci, &impl->appSpace)))
            {
                return MakeError("xr::XrSession: xrCreateReferenceSpace failed");
            }
        }

        // Stereo view configuration -> per-eye extent.
        {
            uint32_t viewCount = 0;
            xrEnumerateViewConfigurationViews(system.Instance(),
                                              system.SystemId(),
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              0,
                                              &viewCount,
                                              nullptr);
            std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            xrEnumerateViewConfigurationViews(system.Instance(),
                                              system.SystemId(),
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              viewCount,
                                              &viewCount,
                                              views.data());
            if (viewCount != 2)
            {
                return MakeError("xr::XrSession: expected 2 stereo views");
            }
            impl->eyeExtent = {views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight};
        }

        // One 2-layer array swapchain: both eyes in one image (multiview).
        int64_t chosenVkFormat = 0;
        {
            uint32_t formatCount = 0;
            xrEnumerateSwapchainFormats(impl->session, 0, &formatCount, nullptr);
            std::vector<int64_t> formats(formatCount);
            xrEnumerateSwapchainFormats(impl->session, formatCount, &formatCount, formats.data());
            for (const int64_t format : formats)
            {
                if (toVriFormat(format) != VriFormat_Unknown)
                {
                    chosenVkFormat    = format;
                    impl->colorFormat = toVriFormat(format);
                    break;
                }
            }
            if (impl->colorFormat == VriFormat_Unknown)
            {
                return MakeError("xr::XrSession: no supported RGBA8/BGRA8 swapchain format");
            }
        }
        {
            XrSwapchainCreateInfo ci {XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format      = chosenVkFormat;
            ci.sampleCount = 1;
            ci.width       = impl->eyeExtent.width;
            ci.height      = impl->eyeExtent.height;
            ci.faceCount   = 1;
            ci.arraySize   = 2;
            ci.mipCount    = 1;
            if (XR_FAILED(xrCreateSwapchain(impl->session, &ci, &impl->swapchain)))
            {
                return MakeError("xr::XrSession: xrCreateSwapchain failed");
            }
        }

        // Wrap each swapchain VkImage as a VriTexture + a state-tracked fg wrapper.
        {
            uint32_t imageCount = 0;
            xrEnumerateSwapchainImages(impl->swapchain, 0, &imageCount, nullptr);
            std::vector<XrSwapchainImageVulkanKHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            xrEnumerateSwapchainImages(
                impl->swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

            impl->wrappedImages.resize(imageCount, nullptr);
            impl->targets.reserve(imageCount);
            for (uint32_t i = 0; i < imageCount; ++i)
            {
                VriWrapTextureDesc wd {};
                wd.nativeTexture  = images[i].image;
                wd.desc.type      = VriTextureType_2DArray;
                wd.desc.format    = impl->colorFormat;
                wd.desc.width     = impl->eyeExtent.width;
                wd.desc.height    = impl->eyeExtent.height;
                wd.desc.depth     = 1;
                wd.desc.mipNum    = 1;
                wd.desc.layerNum  = 2;
                wd.desc.sampleNum = 1;
                wd.desc.usage     = VriTextureUsage_ColorAttachment | VriTextureUsage_ShaderResource;
                if (!Succeeded(interop.WrapTexture(device.Handle(), &wd, &impl->wrappedImages[i])))
                {
                    return MakeError("xr::XrSession: WrapTexture failed");
                }
                impl->targets.push_back(
                    fg::Texture::Borrow(device,
                                        impl->wrappedImages[i],
                                        {.extent = impl->eyeExtent,
                                         .format = impl->colorFormat,
                                         .layers = 2,
                                         .usage  = VriTextureUsage_ColorAttachment | VriTextureUsage_ShaderResource},
                                        {VriAccess_None, VriLayout_Undefined, VriPipelineStage_None}));
            }
        }

        LogInfo("xr: session created ({}x{} per eye, {} swapchain images)",
                impl->eyeExtent.width,
                impl->eyeExtent.height,
                impl->targets.size());
        return std::unique_ptr<XrSession> {new XrSession {std::move(impl)}};
    }

    Expected<StereoFrame> XrSession::BeginFrame()
    {
        auto& impl = *m_impl;
        impl.PollEvents();

        StereoFrame frame;
        frame.colorTarget = nullptr;
        frame.extent      = impl.eyeExtent;

        if (impl.state != SessionState::Running || !impl.sessionBegun)
        {
            return frame; // shouldRender = false; caller keeps pumping (or exits on Exiting)
        }

        XrFrameWaitInfo waitInfo {XR_TYPE_FRAME_WAIT_INFO};
        impl.frameState = {XR_TYPE_FRAME_STATE};
        if (const XrResult r = xrWaitFrame(impl.session, &waitInfo, &impl.frameState); XR_FAILED(r))
        {
            impl.Fail(r, "xrWaitFrame");
            return frame;
        }
        XrFrameBeginInfo beginInfo {XR_TYPE_FRAME_BEGIN_INFO};
        if (const XrResult r = xrBeginFrame(impl.session, &beginInfo); XR_FAILED(r))
        {
            impl.Fail(r, "xrBeginFrame");
            return frame;
        }
        impl.frameBegun = true;

        if (!impl.frameState.shouldRender)
        {
            return frame; // EndFrame() must still run to pair xrBeginFrame
        }

        // Locate both eye views for this frame's predicted display time.
        {
            XrViewState      viewState {XR_TYPE_VIEW_STATE};
            uint32_t         count = 0;
            XrViewLocateInfo li {XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime           = impl.frameState.predictedDisplayTime;
            li.space                 = impl.appSpace;
            impl.locatedViews[0]     = {XR_TYPE_VIEW};
            impl.locatedViews[1]     = {XR_TYPE_VIEW};
            xrLocateViews(impl.session, &li, &viewState, 2, &count, impl.locatedViews);
        }

        // Acquire the 2-layer image; xrWaitSwapchainImage ensures it is safe to write.
        {
            uint32_t index = 0;
            if (XR_FAILED(xrAcquireSwapchainImage(impl.swapchain, nullptr, &index)))
            {
                return frame;
            }
            XrSwapchainImageWaitInfo waitImage {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitImage.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(impl.swapchain, &waitImage);
            impl.acquiredIndex = index;
            impl.imageAcquired = true;
        }

        // Contents are undefined on acquire; first write transitions from Undefined.
        auto& target = impl.targets[impl.acquiredIndex];
        target.state = {VriAccess_None, VriLayout_Undefined, VriPipelineStage_None};

        frame.shouldRender   = true;
        frame.displayTimeSec = static_cast<double>(impl.frameState.predictedDisplayTime) * 1e-9;
        frame.colorTarget    = &target;
        frame.views.nearZ    = impl.nearZ;
        frame.views.farZ     = impl.farZ;
        for (uint32_t eye = 0; eye < 2; ++eye)
        {
            auto& v = frame.views.eye[eye];
            v.pose  = fromXr(impl.locatedViews[eye].pose);
            v.fov   = fromXr(impl.locatedViews[eye].fov);
            v.view  = ViewFromPose(v.pose);
            v.proj  = ProjectionFromFov(v.fov, impl.nearZ, impl.farZ);
        }
        return frame;
    }

    void XrSession::PreSubmit(VriCommandBuffer* cmd, StereoFrame& frame)
    {
        // OpenXR expects the released color image in COLOR_ATTACHMENT layout.
        auto& impl = *m_impl;
        if (!frame.shouldRender || !frame.colorTarget)
        {
            return;
        }
        auto&                      target = *frame.colorTarget;
        const VriAccessLayoutStage wanted {
            VriAccess_ColorAttachmentWrite, VriLayout_ColorAttachment, VriPipelineStage_ColorAttachmentOutput};
        if (target.state.layout == wanted.layout)
        {
            return;
        }
        const VriTextureBarrierDesc barrier {
            .texture = target.Handle(),
            .before  = target.state,
            .after   = wanted,
            .aspect  = VriImageAspect_Color,
        };
        const VriBarrierGroupDesc group {.textures = &barrier, .textureNum = 1};
        impl.device->Core().CmdBarrier(cmd, &group);
        target.state = wanted;
    }

    void XrSession::EndFrame(const StereoFrame& frame)
    {
        auto& impl = *m_impl;
        if (!impl.frameBegun)
        {
            return;
        }

        XrCompositionLayerProjectionView projViews[2] {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                       {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection     layer {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        bool                             haveLayer = false;

        if (frame.shouldRender && impl.imageAcquired)
        {
            xrReleaseSwapchainImage(impl.swapchain, nullptr);
            impl.imageAcquired = false;

            for (uint32_t eye = 0; eye < 2; ++eye)
            {
                projViews[eye].pose                      = impl.locatedViews[eye].pose;
                projViews[eye].fov                       = impl.locatedViews[eye].fov;
                projViews[eye].subImage.swapchain        = impl.swapchain;
                projViews[eye].subImage.imageRect.offset = {0, 0};
                projViews[eye].subImage.imageRect.extent = {static_cast<int32_t>(impl.eyeExtent.width),
                                                            static_cast<int32_t>(impl.eyeExtent.height)};
                projViews[eye].subImage.imageArrayIndex  = eye;
            }
            layer.space     = impl.appSpace;
            layer.viewCount = 2;
            layer.views     = projViews;
            haveLayer       = true;
        }

        XrFrameEndInfo endInfo {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime                          = impl.frameState.predictedDisplayTime;
        endInfo.environmentBlendMode                 = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        endInfo.layerCount                           = haveLayer ? 1u : 0u;
        endInfo.layers                               = haveLayer ? layers : nullptr;
        xrEndFrame(impl.session, &endInfo);
        impl.frameBegun = false;
    }

    Extent2D  XrSession::EyeExtent() const { return m_impl->eyeExtent; }
    VriFormat XrSession::ColorFormat() const { return m_impl->colorFormat; }

    bool XrSession::IsRunning() const { return m_impl->state != SessionState::Exiting; }

    SessionState XrSession::State() const { return m_impl->state; }

    void XrSession::RequestExit()
    {
        if (m_impl->session != XR_NULL_HANDLE)
        {
            xrRequestExitSession(m_impl->session);
        }
    }

    XrSessionHandles XrSession::Handles() const
    {
        return XrSessionHandles {
            .instance             = m_impl->instance,
            .session              = m_impl->session,
            .appSpace             = m_impl->appSpace,
            .predictedDisplayTime = m_impl->frameState.predictedDisplayTime,
        };
    }
} // namespace vrf::xr

#endif // VRF_WITH_OPENXR
