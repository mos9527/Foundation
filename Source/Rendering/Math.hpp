#pragma once
#include <Math/Math.hpp>
#include <fmt/format.h>
namespace glm
{
    inline auto format_as(mat4 m)
    {
        return fmt::format("mat4(\n"
                           "  {},{},{},{},\n"
                           "  {},{},{},{},\n"
                           "  {},{},{},{},\n"
                           "  {},{},{},{}\n"
                           ")",
                           m[0][0], m[0][1], m[0][2], m[0][3],
                           m[1][0], m[1][1], m[1][2], m[1][3],
                           m[2][0], m[2][1], m[2][2], m[2][3],
                           m[3][0], m[3][1], m[3][2], m[3][3]
                           );
    }
}
namespace Foundation::Math
{
    // zNear -> zNDC = 1, zFar (inf) -> zNDC = 0
    // Right = +X, Up = +Y, Forward = +Z
    inline mat4 infinitePerspectiveRHReverseZ(float fovY /* radians */, float a /* aspect W/H */, float zNear)
    {
        float f = 1.0f / tanf(fovY / 2.0f);
        return{
            f / a, 0.0f, 0.0f, 0.0f,
            0.0f, f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, zNear, 0.0f
        };
    }

    /** @ref infinitePerspectiveLHReverseZ **/
    inline mat4 perspectiveRHReverseZ(float fovY, float a, float zNear, float zFar)
    {
        float f = 1.0f / tanf(fovY / 2.0f);
        return {
            f / a, 0.0f, 0.0f, 0.0f,
            0.0f, f, 0.0f, 0.0f,
            0,0,-zNear/(zFar - zNear),1,
            0,0,zFar * zNear/(zFar - zNear),0
        };
    }

    // Forward = +Z
    inline mat4 viewMatrixPosRot(vec3 pos, quat rot)
    {
        mat4 view = mat4_cast(normalize(rot));
        view[3] = vec4(pos,1.0f);
        view = inverse(view);
        view = scale(glm::identity<mat4>(), vec3(1, 1, -1)) * view;
        return view;
    }
}