/*
 * device_info.hpp - a human-readable dump of the render device's capabilities.
 *
 * DescribeDevice() formats the VriDeviceDesc that VRI reports for the selected
 * backend/GPU (adapter, API version, memory, queues, limits and the full
 * optional-feature matrix) into a multi-line string. Examples print it at
 * startup to show what the chosen backend can actually do.
 */
#pragma once

#include <string>

namespace vrf
{
    class RenderDevice;

    // Multi-line, human-readable summary of the device: adapter + vendor/type, API and
    // version, video memory, queues, key resource limits, and which optional VRI features
    // (ray tracing, mesh shaders, bindless, VRS, wave ops, multiview, ...) are supported.
    [[nodiscard]] std::string DescribeDevice(const RenderDevice& device);
} // namespace vrf
