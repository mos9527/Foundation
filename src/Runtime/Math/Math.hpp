#pragma once
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#define GLM_FORCE_RADIANS
#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

/**
 * @brief Mathematical utilities and types.
 */
namespace Foundation::Math {
    using namespace glm;
    using float4 = vec4;
    // !! TODO: Padding?
    using float3 = vec3;
    using float2 = vec2;
    using float4x4 = mat4;
}

#include "Quantization.hpp"
#include "Unorm.hpp"
