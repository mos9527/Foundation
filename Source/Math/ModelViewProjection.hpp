#pragma once
#include "Math.hpp"
namespace Foundation::Math
{
    // zNear -> zNDC = 1, zFar (inf) -> zNDC = 0
    // View perspective: Right = +X, Up = +Y, Forward = -Z
    inline mat4 infinitePerspectiveRHReverseZ(float fovY /* radians */, float a /* aspect W/H */, float zNear)
    {
        float f = 1 / tan(fovY / 2);
        return mat4{f / a, 0, 0, 0, 0, f, 0, 0,
                    // zNDC = zNear/z
                    0, 0, 0, -1, 0, 0, zNear, 0};
    }

    // zNear -> zNDC = 1, zFar -> zNDC = 0
    // View perspective: Right = +X, Up = +Y, Forward = -Z
    inline mat4 perspectiveRHReverseZ(float fovY, float a, float zNear, float zFar)
    {
        float f = 1 / tan(fovY / 2);
        return mat4{f / a, 0, 0, 0, 0, f, 0, 0,
                    // zNDC = zNear/z
                    0, 0, zNear / (zFar - zNear), -1, 0, 0, (zFar * zNear) / (zFar - zNear), 0};
    }

    // Forward = -Z
    inline mat4 viewMatrixRHReverseZ(vec3 pos, quat rot)
    {
        mat4 view = mat4_cast(rot);
        view[3] = vec4(pos.x, pos.y, pos.z, 1.0f);
        return inverse(view);
    }

    // (i,j,k,l), where left/right planes are ix +- jz = 0, top/bottom planes are ky +- lz = 0
    inline float4 planeSymmetric(mat4 proj)
    {
        mat4 projT = transpose(proj);
        float4 left = projT[3] + projT[0]; // (m41 + m11, m42 + m12, m43 + m13, m44 + m14)
        float4 bottom = projT[3] + projT[1]; // (m41 + m21, m42 + m22, m43 + m23, m44 + m24)
        // Normalize
        left /= length(left.xyz());
        bottom /= length(bottom.xyz());
        return {left.x, left.z, bottom.y, bottom.z};
    }
} // namespace Foundation::Math
