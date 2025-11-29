#pragma once
#include "Math.hpp"
namespace Foundation::Math
{
    inline bool decompose(mat4 const& m, float3& scale, quat& rotation, float3& transform)
    {
        /**
        * Row-major. glm decompose fails to solve this one's perspective component correctly (FIXME in its docs).
        * 0.0014 0 0 0
        * 0 0 0.0014 0
        * 0 -0.0014 0 0
        * 2 0 2 1
        * Hence the implementation for *only* the TRS components. Assumes the matrix is affine.
        */
        mat3 basis = mat3(m);
        transform = { m[3][0],m[3][1],m[3][2] };
        // Negative determinant means handness flip
        float det = determinant(basis);
        float sgn = det > 0 ? 1 : -1;
        scale = {
            length(float3{ basis[0][0], basis[0][1], basis[0][2] }) * sgn,
            length(float3{ basis[1][0], basis[1][1], basis[1][2] }) * sgn,
            length(float3{ basis[2][0], basis[2][1], basis[2][2] }) * sgn
        };
        // Normalize to get pure rotation matrix
        basis[0] /= scale.x == .0f ? 1.0f : scale.x;
        basis[1] /= scale.y == .0f ? 1.0f : scale.y;
        basis[2] /= scale.z == .0f ? 1.0f : scale.z;
        rotation = quat_cast(basis);
        return true;
    }
}