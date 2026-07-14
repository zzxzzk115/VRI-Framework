#if defined(VRF_WITH_IMGUI)

#include "vrf/gpu/imgui_backend.hpp"

#include <utility>

#include <imgui.h>

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    ImGuiBackend::~ImGuiBackend() { Reset(); }

    ImGuiBackend::ImGuiBackend(ImGuiBackend&& other) noexcept :
        m_device {std::exchange(other.m_device, nullptr)}, m_api {other.m_api},
        m_gui {std::exchange(other.m_gui, nullptr)}, m_fontView {std::exchange(other.m_fontView, nullptr)},
        m_hasData {other.m_hasData}, m_vertices {std::move(other.m_vertices)}, m_indices {std::move(other.m_indices)},
        m_commands {std::move(other.m_commands)}, m_data {other.m_data}
    {}

    ImGuiBackend& ImGuiBackend::operator=(ImGuiBackend&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_device   = std::exchange(other.m_device, nullptr);
            m_api      = other.m_api;
            m_gui      = std::exchange(other.m_gui, nullptr);
            m_fontView = std::exchange(other.m_fontView, nullptr);
            m_hasData  = other.m_hasData;
            m_vertices = std::move(other.m_vertices);
            m_indices  = std::move(other.m_indices);
            m_commands = std::move(other.m_commands);
            m_data     = other.m_data;
        }
        return *this;
    }

    void ImGuiBackend::Reset() noexcept
    {
        if (m_device && m_gui)
        {
            m_api.DestroyImgui(m_gui);
        }
        m_gui      = nullptr;
        m_fontView = nullptr;
        m_device   = nullptr;
    }

    Expected<ImGuiBackend> ImGuiBackend::Create(RenderDevice&  device,
                                                const VriFormat colorFormat,
                                                const VriFormat depthFormat,
                                                const void*     fontPixels,
                                                const uint32_t  fontWidth,
                                                const uint32_t  fontHeight)
    {
        ImGuiBackend out;
        out.m_device = &device;

        if (const auto r = vriGetInterface(device.Handle(), VRI_INTERFACE_IMGUI, sizeof(out.m_api), &out.m_api);
            !Succeeded(r))
        {
            return MakeError(r, "ImGuiBackend::Create", "imgui interface unavailable");
        }

        VriImguiDesc desc {};
        desc.uploadQueue = device.GraphicsQueue();
        desc.colorFormat = colorFormat;
        desc.depthFormat = depthFormat;
        desc.fontAtlas   = fontPixels;
        desc.fontWidth   = fontWidth;
        desc.fontHeight  = fontHeight;
        if (const auto r = out.m_api.CreateImgui(device.Handle(), &desc, &out.m_gui); !Succeeded(r))
        {
            return MakeError(r, "ImGuiBackend::Create", "CreateImgui failed");
        }
        out.m_fontView = out.m_api.GetImguiFontView(out.m_gui);
        return out;
    }

    void ImGuiBackend::Upload(ImDrawData* drawData, const Extent2D framebufferExtent)
    {
        // Flatten ImDrawData (per-list buffers + list-relative offsets) into the
        // neutral VriImguiDrawData (one buffer each, global offsets).
        m_vertices.clear();
        m_indices.clear();
        m_commands.clear();
        m_data    = {};
        m_hasData = false;
        if (!drawData || drawData->TotalVtxCount == 0)
        {
            return;
        }

        m_vertices.reserve(static_cast<size_t>(drawData->TotalVtxCount));
        m_indices.reserve(static_cast<size_t>(drawData->TotalIdxCount));
        uint32_t vertexBase = 0, indexBase = 0;
        for (int n = 0; n < drawData->CmdListsCount; ++n)
        {
            const ImDrawList* list = drawData->CmdLists[n];
            for (int v = 0; v < list->VtxBuffer.Size; ++v)
            {
                const ImDrawVert& s = list->VtxBuffer.Data[v];
                m_vertices.push_back(VriImguiVertex {{s.pos.x, s.pos.y}, {s.uv.x, s.uv.y}, s.col});
            }
            for (int i = 0; i < list->IdxBuffer.Size; ++i)
            {
                m_indices.push_back(static_cast<uint16_t>(list->IdxBuffer.Data[i]));
            }
            for (int i = 0; i < list->CmdBuffer.Size; ++i)
            {
                const ImDrawCmd& dc = list->CmdBuffer[i];
                if (dc.UserCallback)
                {
                    dc.UserCallback(list, &dc);
                    continue;
                }
                VriImguiDrawCommand o {};
                o.clipRect[0]  = dc.ClipRect.x;
                o.clipRect[1]  = dc.ClipRect.y;
                o.clipRect[2]  = dc.ClipRect.z;
                o.clipRect[3]  = dc.ClipRect.w;
                o.indexCount   = dc.ElemCount;
                o.indexOffset  = indexBase + dc.IdxOffset;
                o.vertexOffset = static_cast<int32_t>(vertexBase + dc.VtxOffset);
                o.textureView  = reinterpret_cast<VriDescriptor*>(dc.GetTexID());
                m_commands.push_back(o);
            }
            vertexBase += static_cast<uint32_t>(list->VtxBuffer.Size);
            indexBase += static_cast<uint32_t>(list->IdxBuffer.Size);
        }

        m_data.vertices          = m_vertices.data();
        m_data.vertexCount       = static_cast<uint32_t>(m_vertices.size());
        m_data.indices           = m_indices.data();
        m_data.indexCount        = static_cast<uint32_t>(m_indices.size());
        m_data.indexSize         = sizeof(uint16_t);
        m_data.commands          = m_commands.data();
        m_data.commandCount      = static_cast<uint32_t>(m_commands.size());
        m_data.displayPos[0]     = drawData->DisplayPos.x;
        m_data.displayPos[1]     = drawData->DisplayPos.y;
        m_data.displaySize[0]    = drawData->DisplaySize.x;
        m_data.displaySize[1]    = drawData->DisplaySize.y;
        m_data.framebufferWidth  = framebufferExtent.width;
        m_data.framebufferHeight = framebufferExtent.height;

        m_api.UploadImguiData(m_gui, &m_data);
        m_hasData = true;
    }

    void ImGuiBackend::CmdCopy(VriCommandBuffer* cmd)
    {
        if (m_hasData)
        {
            m_api.CmdCopyImguiData(cmd, m_gui);
        }
    }

    void ImGuiBackend::CmdDraw(VriCommandBuffer* cmd)
    {
        if (m_hasData)
        {
            m_api.CmdDrawImgui(cmd, m_gui, &m_data);
        }
    }
} // namespace vrf

#endif // VRF_WITH_IMGUI
