/**
 * Unit Vector Packing
 * See also:
 * - https://aras-p.info/texts/CompactNormalStorage.html
 * - https://www.shadertoy.com/view/Mtfyzl
 */
#pragma once
#include "Math.hpp"
namespace Foundation::Math {
    using namespace glm;
    inline vec2 packUnitOctahedral(vec3 v) {
        v /= vec3(fabsf(v.x) + fabsf(v.y) + fabsf(v.z));
        v.xy() = (v.z >= 0.0f) ? v.xy() : (vec2(1.0f) - abs(vec2(v.yx()))) * sign(vec2(v.xy()));
        return v.xy();
    }
    inline vec3 unpackUnitOctahedral(vec2 e) {
        vec3 v = vec3(e.xy(), 1.0f - fabsf(e.x) - fabsf(e.y));
        v.xy() += vec2(max(-v.z, 0.0f)) * (-1.0f * sign(vec2(v.xy())));
        return normalize(v);
    }
}
