#pragma once
#include <Math/Math.hpp>
#include <cfloat>

using namespace Foundation::Math;

struct FSerializedBounds
{
    float3 min{0.0f, 0.0f, 0.0f};
    float3 max{0.0f, 0.0f, 0.0f};

    static FSerializedBounds Empty()
    {
        return {
            .min = float3(FLT_MAX),
            .max = float3(-FLT_MAX),
        };
    }

    [[nodiscard]] bool IsValid() const
    {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    FSerializedBounds& Include(float3 const& point)
    {
        min = Foundation::Math::min(min, point);
        max = Foundation::Math::max(max, point);
        return *this;
    }

    FSerializedBounds& Include(FSerializedBounds const& bounds)
    {
        if (!bounds.IsValid())
            return *this;
        Include(bounds.min);
        Include(bounds.max);
        return *this;
    }

    FSerializedBounds& operator+=(float3 const& point)
    {
        return Include(point);
    }

    FSerializedBounds& operator+=(FSerializedBounds const& bounds)
    {
        return Include(bounds);
    }
};
