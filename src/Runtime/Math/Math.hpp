#pragma once
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#define GLM_FORCE_RADIANS
#define GLM_FORCE_SWIZZLE 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <bitset>

#include <meshoptimizer.h>
namespace Foundation::Math {
    using namespace glm;
/// IEEE float & fixed point (de)quantization implmentations from https://github.com/zeux/meshoptimizer
/// See also
/// - https://fgiesen.wordpress.com/2024/11/06/exact-unorm8-to-float/
#pragma region Quantization
    // Quantize FP32 with fixed mantissa precision
    inline float QuantizeFP32(float v, int32_t N) {
        return meshopt_quantizeFloat(v, N);
    }
    // Quantize FP32 into IEEE FP16
    inline uint16_t QuantizeFP16(float v) {
        return meshopt_quantizeHalf(v);
    }
    // Dequantize FP16 into IEEE FP32
    inline float DequantizeFP16(uint16_t h) {
        return meshopt_dequantizeHalf(h);
    }
    // [0,1] range -> [0, 1 << NBits) \in N
    inline uint32_t QuantizeUnorm(float v, int32_t Nbits) {
        return meshopt_quantizeUnorm(v, Nbits);
    }
    // [0, 1 << NBits) \in N -> [0, 1] range
    inline float DequantizeUnorm(int32_t q, int32_t Nbits) {
        return q / (float)((1 << Nbits) - 1);
    }
    // [-1, 1] range -> [-(1<< (Nbits - 1)) - 1, (1 << (Nbits - 1))] \in N
    // e.g. Nbits = 10 -> [-511, 512]
    // In transport you may want to add 1 << (Nbits - 1) to the quantized value to shift it to [0, 1 << NBits) range
    // since you'd be packing complement int32 bits - truncation would result in a loss of precision
    // To do this, use QuantizeSnormShifted and DequantizeSnormShifted
    inline int32_t QuantizeSnorm(float v, int32_t Nbits) {
        return meshopt_quantizeSnorm(v, Nbits);
    }
    // See also QuantizeSnorm
    // [-(1<< (Nbits - 1)) - 1, (1 << (Nbits - 1))] \in N -> [-1, 1]
    inline float DequantizeSnorm(int32_t q, int32_t Nbits) {
        return q / (float)((1 << (Nbits - 1)) - 1);
    }
    // [-1, 1] range -> [0, 1 << NBits) \in N
    inline uint32_t QuantizeSnormShifted(float v, int32_t Nbits) {        
        return QuantizeSnorm(v, Nbits) + (1 << (Nbits - 1));
    }
    // See also QuantizeSnorm
    // [0, 1 << NBits) \in N -> [-1, 1] range
    inline float DequantizeSnormShifted(uint32_t q, int32_t Nbits) {
        return DequantizeSnorm(q - (1 << (Nbits - 1)), Nbits);
    }
#pragma endregion

/// (Unit) Vector Packing
/// See also:
/// - https://aras-p.info/texts/CompactNormalStorage.html
/// - https://www.shadertoy.com/view/Mtfyzl
#pragma region Vector Packing
    inline vec2 PackUnitOctahedral(vec3 v) {
        v /= vec3(fabsf(v.x) + fabsf(v.y) + fabsf(v.z));
        v.xy = (v.z >= 0.0f) ? v.xy : (vec2(1.0f) - abs(vec2(v.yx))) * sign(vec2(v.xy));
        return v.xy;
    }
    inline vec3 UnpackUnitOctahedral(vec2 e) {
        vec3 v = vec3(e.xy, 1.0f - fabsf(e.x) - fabsf(e.y));
        v.xy += vec2(max(-v.z, 0.0f)) * (-1.0f * sign(vec2(v.xy)));
        return normalize(v);
    }
#pragma endregion
}

