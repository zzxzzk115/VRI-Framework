#include "vrf/gpu/builders/buffer_builder.hpp"

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    Expected<VriBuffer*> BufferBuilder::Build(RenderDevice& device) const
    {
        VriBuffer*      buffer = nullptr;
        const VriResult r      = device.Core().CreateBuffer(device.Handle(), &m_desc, &buffer);
        if (r != VriResult_Success)
            return MakeError(r, "BufferBuilder::Build", "CreateBuffer failed");
        return buffer;
    }
} // namespace vrf
