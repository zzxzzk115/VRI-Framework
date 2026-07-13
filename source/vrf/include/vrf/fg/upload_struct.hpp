/*
 * upload_struct.hpp - one-shot upload of a CPU struct into a transient buffer,
 * as its own framegraph pass. Uniform buffers live host-visible under vrf::fg,
 * so the "upload" is a map-write at pass execution (ordered before any
 * consuming pass by graph dependency).
 */
#pragma once

#include <memory>

#include <fg/FrameGraph.hpp>

#include "vrf/fg/framegraph_buffer.hpp"
#include "vrf/fg/render_context.hpp"
#include "vrf/fg/resource_access.hpp"
#include "vrf/fg/transient_buffer.hpp"

namespace vrf::fg
{
    template<typename T>
    [[nodiscard]] FrameGraphResource uploadStruct(FrameGraph& fg, const std::string_view passName, TransientBuffer<T>&& s)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        constexpr auto kDataSize = static_cast<uint32_t>(sizeof(T));

        auto payload = std::make_shared<T>(std::move(s.data));

        struct Data
        {
            FrameGraphResource buffer;
        };
        const auto [buffer] = fg.addCallbackPass<Data>(
            passName,
            [&s](FrameGraph::Builder& builder, Data& data) {
                data.buffer = builder.create<FrameGraphBuffer>(s.name,
                                                               {
                                                                   .type     = s.type,
                                                                   .stride   = kDataSize,
                                                                   .capacity = 1,
                                                               });
                data.buffer = builder.write(data.buffer, BindingInfo {.pipelineStage = PipelineStage::Transfer});
            },
            [payload](const Data& data, FrameGraphPassResources& resources, void* ctx) {
                auto& rc = *static_cast<RenderContext*>(ctx);
                auto* buffer = resources.get<FrameGraphBuffer>(data.buffer).buffer;
                buffer->Write(0, kDataSize, payload.get());
                (void) rc;
            });

        return buffer;
    }
} // namespace vrf::fg
