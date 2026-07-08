/*
 * math.hpp - glm type aliases and small geometry helpers used across the
 * framework's public API.
 */
#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace vrf
{
    using Vec2  = glm::vec2;
    using Vec3  = glm::vec3;
    using Vec4  = glm::vec4;
    using IVec2 = glm::ivec2;
    using IVec4 = glm::ivec4;
    using UVec2 = glm::uvec2;
    using Mat3  = glm::mat3;
    using Mat4  = glm::mat4;
    using Quat  = glm::quat;

    // A 2D size in pixels (window / swapchain / texture extent).
    struct Extent2D
    {
        uint32_t width  = 0;
        uint32_t height = 0;

        [[nodiscard]] constexpr bool operator==(const Extent2D&) const noexcept = default;
        [[nodiscard]] constexpr bool IsZero() const noexcept { return width == 0 || height == 0; }
    };
} // namespace vrf
