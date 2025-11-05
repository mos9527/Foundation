#pragma once
#include <Math/Math.hpp>

namespace Foundation::Math
{
    // zNear -> zNDC = 1, zFar (inf) -> zNDC = 0
    // Right = +X, Up = +Y, Forward = +Z
    inline mat4 infinitePerspectiveLHReverseZ(float fovY /* radians */, float a /* aspect W/H */, float zNear)
    {
        float f = 1 / tan(fovY / 2);
        // Col Major!!!
        return mat4{
            f/a,0,0,0,
            0,f,0,0,
            // zNDC = zNear/z
            0,0,0,1,
            0,0,zNear,0
        };
    }

    /** @ref infinitePerspectiveLHReverseZ **/
    inline mat4 perspectiveLHReverseZ(float fovY, float a, float zNear, float zFar)
    {
        float f = 1 / tan(fovY / 2);
        return mat4{
            f/a,0,0,0,
            0,f,0,0,
            // zNDC = zNear/z
            0,0,-zNear/(zFar - zNear),1,
            0,0,(zFar * zNear)/(zFar - zNear),0
        };
    }

    // Forward = +Z
    inline mat4 viewMatrixLH(vec3 pos, quat rot)
    {
        constexpr mat4 kZplus = mat4{
            1,0,0,0,
            0,1,0,0,
            0,0,-1,0,
            0,0,0,1
        };
        mat4 view = mat4_cast(rot);
        view[3] = vec4(pos.x,pos.y,pos.z,1.0f);
        view = inverse(view);
        return kZplus * view;
    }
}