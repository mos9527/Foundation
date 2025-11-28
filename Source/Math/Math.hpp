#pragma once
#define GLM_FORCE_RADIANS
#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL
#define GLM_FORCE_QUAT_DATA_XYZW
#define GLM_FORCE_XYZW_ONLY
#define GLM_FORCE_QUAT_CTOR_XYZW
#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/integer.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_transform.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat2x2.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>

namespace Foundation::Math {
    using namespace glm;
    using float4 = vec4;
    using float3 = vec3;
    using float2 = vec2;
    using float4x4 = mat4;
    // No Surprises.
    // Shaders are compiled with -fvk-use-scalar-layout
    // so interexchange should always be dense
    static_assert(sizeof(float4) == 4 * sizeof(float));
    static_assert(sizeof(float3) == 3 * sizeof(float));
    static_assert(sizeof(float2) == 2 * sizeof(float));
    static_assert(sizeof(float4x4) == 16 * sizeof(float));

#pragma region MVP
    // zNear -> zNDC = 1, zFar (inf) -> zNDC = 0
    // View perspective: Right = +X, Up = +Y, Forward = -Z
    inline mat4 infinitePerspectiveRHReverseZ(float fovY /* radians */, float a /* aspect W/H */, float zNear)
    {
        float f = 1 / tan(fovY / 2);
        return mat4{
            f/a,0,0,0,
            0,f,0,0,
            // zNDC = zNear/z
            0,0,0,-1,
            0,0,zNear,0
        };
    }

    // zNear -> zNDC = 1, zFar -> zNDC = 0
    // View perspective: Right = +X, Up = +Y, Forward = -Z
    inline mat4 perspectiveRHReverseZ(float fovY, float a, float zNear, float zFar)
    {
        float f = 1 / tan(fovY / 2);
        return mat4{
            f/a,0,0,0,
            0,f,0,0,
            // zNDC = zNear/z
            0,0,zNear/(zFar - zNear),-1,
            0,0,(zFar * zNear)/(zFar - zNear),0
        };
    }

    // Forward = -Z
    inline mat4 viewMatrixRHReverseZ(vec3 pos, quat rot)
    {
        mat4 view = mat4_cast(rot);
        view[3] = vec4(pos.x,pos.y,pos.z,1.0f);
        return inverse(view);
    }
#pragma endregion

#pragma region TRS
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
