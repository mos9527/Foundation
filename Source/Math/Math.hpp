#pragma once
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
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
    // XXX: vec3 padding can be quite broken
    //      Currently we compile shaders with -fvk-use-scalar-layout to
    //      always densely pack them.
    // TODO: This may have performance implications on some hardware - needs testing.
    // TODO: MSVC default alignment is a bit weird. Figure out how to make it compilant.
    using float3 = vec3;
    using float2 = vec2;
    using float4x4 = mat4;
}

#include "Quantization.hpp"
#include "Unorm.hpp"
