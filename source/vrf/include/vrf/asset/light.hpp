/*
 * light.hpp - a simple analytic light source (framework-level; loader-agnostic).
 */
#pragma once

#include <numbers>
#include <string>
#include <utility>

#include "vrf/core/math.hpp"

namespace vrf
{
    enum class LightKind
    {
        Directional,
        Point,
        Spot,
        Area
    };

    struct Light
    {
        std::string name;

        LightKind kind = LightKind::Directional;

        Vec3  position  = {0.0f, 0.0f, 0.0f};
        Vec3  direction = {0.0f, -1.0f, 0.0f};
        Vec3  color     = {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float range     = 0.0f; // 0 = infinite

        // Spot cone half-angles (radians).
        float innerConeAngle = 0.0f;
        float outerConeAngle = std::numbers::pi_v<float> / 4.0f;

        // Point / area soft-shadow radius (area: half-extent).
        float radius = 0.0f;
    };

    // Fluent builder for hand-authoring a Light.
    class LightBuilder
    {
    public:
        LightBuilder& SetName(std::string name)
        {
            m_light.name = std::move(name);
            return *this;
        }
        LightBuilder& SetKind(LightKind kind)
        {
            m_light.kind = kind;
            return *this;
        }
        LightBuilder& SetPosition(const Vec3& position)
        {
            m_light.position = position;
            return *this;
        }
        LightBuilder& SetDirection(const Vec3& direction)
        {
            m_light.direction = direction;
            return *this;
        }
        LightBuilder& SetColor(const Vec3& color)
        {
            m_light.color = color;
            return *this;
        }
        LightBuilder& SetIntensity(float intensity)
        {
            m_light.intensity = intensity;
            return *this;
        }
        LightBuilder& SetRange(float range)
        {
            m_light.range = range;
            return *this;
        }
        LightBuilder& SetSpotCone(float innerRadians, float outerRadians)
        {
            m_light.innerConeAngle = innerRadians;
            m_light.outerConeAngle = outerRadians;
            return *this;
        }
        LightBuilder& SetRadius(float radius)
        {
            m_light.radius = radius;
            return *this;
        }

        [[nodiscard]] Light Build() const { return m_light; }

    private:
        Light m_light;
    };
} // namespace vrf
