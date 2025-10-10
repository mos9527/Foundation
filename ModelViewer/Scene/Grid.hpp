#pragma once
#include <Math/Math.hpp>
#include "Camera.hpp"
namespace ModelViewer
{
    struct Grid
    {
        uint32_t dimension{10000};
        float width{0.01};
        enum class Type : uint32_t
        {
            Cartesian = 0,
            Radial = 1
        } type{Type::Cartesian};

        struct Params
        {
            Camera::Params camera;
            uint dimension;
            float width;
            uint type;
        };
        Params GetParams(Camera const& camera) const;
    };
}