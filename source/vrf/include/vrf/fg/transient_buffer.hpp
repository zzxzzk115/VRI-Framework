/*
 * transient_buffer.hpp - named per-frame data blob declaration, consumed by
 * uploadStruct (upload_struct.hpp).
 */
#pragma once

#include <string_view>

#include "vrf/fg/buffer.hpp"

namespace vrf::fg
{
#if defined(_MSC_VER)
#pragma warning(push)
// structure was padded due to alignment specifier
#pragma warning(disable : 4324)
#endif

    template<typename T>
    struct TransientBuffer
    {
        const std::string_view name;
        BufferType             type {BufferType::UniformBuffer};
        T                      data;
    };

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
} // namespace vrf::fg
