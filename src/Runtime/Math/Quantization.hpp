/**
* Float (de)quantization code taken from meshoptimizer (https://github.com/zeux/meshoptimizer)
* 
* All rights reserved by the original author, see license in meshoptimizer.h
*/
#pragma once
#include <cstdint>
#include <bitset>
namespace Foundation::Math {
    /**
     * Quantize a float into a floating point value with a limited number of significant mantissa bits, preserving the IEEE-754 fp32 binary representation
     * Generates +-inf for overflow, preserves NaN, flushes denormals to zero, rounds to nearest
     * Assumes N is in a valid mantissa precision range, which is 1..23
     */
    float quantizeFP32(float v, int32_t N);

    /**
     * Quantize a float into half-precision (as defined by IEEE-754 fp16) floating point value
     * Generates +-inf for overflow, preserves NaN, flushes denormals to zero, rounds to nearest
     * Representable magnitude range: [6e-5; 65504]
     * Maximum relative reconstruction error: 5e-4
     */
    uint16_t quantizeFP16(float v);

    /**
     * Reverse quantization of a half-precision (as defined by IEEE-754 fp16) floating point value
     * Preserves Inf/NaN, flushes denormals to zero
     */
    float dequantizeFP16(uint16_t h);

    /* [0,1] range -> [0, 1 << NBits) \in N */
    inline uint32_t quantizeUnorm(float v, int32_t N) {
        const float scale = static_cast<float>((1 << N) - 1);

        v = (v >= 0) ? v : 0;
        v = (v <= 1) ? v : 1;

        return static_cast<int>(v * scale + 0.5f);
    }

    /* [0, 1 << NBits) \in N -> [0, 1] range */
    inline float dequantizeUnorm(int32_t q, int32_t Nbits) {
        return q / static_cast<float>((1 << Nbits) - 1);
    }

    /**
     * [-1, 1] range -> [-(1<< (Nbits - 1)) - 1, (1 << (Nbits - 1))] \in N
     * e.g. Nbits = 10 -> [-511, 512]
     * In transport you may want to add 1 << (Nbits - 1) to the quantized value to shift it to [0, 1 << NBits) range
     * since you'd be packing complement int32 bits - truncation would result in a loss of precision
     * To do this, use QuantizeSnormShifted and DequantizeSnormShifted
    */
    inline int32_t quantizeSnorm(float v, int32_t N) {
        const float scale = static_cast<float>((1 << (N - 1)) - 1);

        float round = (v >= 0 ? 0.5f : -0.5f);

        v = (v >= -1) ? v : -1;
        v = (v <= +1) ? v : +1;

        return static_cast<int>(v * scale + round);
    }

    // [-(1<< (Nbits - 1)) - 1, (1 << (Nbits - 1))] \in N -> [-1, 1]
    inline float dequantizeSnorm(int32_t q, int32_t Nbits) {
        return q / static_cast<float>((1 << (Nbits - 1)) - 1);
    }

    // [-1, 1] range -> [0, 1 << NBits) \in N
    inline uint32_t quantizeSnormShifted(float v, int32_t Nbits) {
        return quantizeSnorm(v, Nbits) + (1 << (Nbits - 1));
    }

    // [0, 1 << NBits) \in N -> [-1, 1] range
    inline float dequantizeSnormShifted(uint32_t q, int32_t Nbits) {
        return dequantizeSnorm(q - (1 << (Nbits - 1)), Nbits);
    }
}
