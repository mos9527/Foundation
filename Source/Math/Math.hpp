#pragma once
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat2x2.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
/**
 * @brief Mathematical utilities and types.
 */
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
}
#include "Quantization.hpp"
#include "Unorm.hpp"
