#include "Unorm.hpp"
namespace Foundation::Math
{
    vec2 packUnitOctahedral(vec3 v) {
        v /= vec3(fabsf(v.x) + fabsf(v.y) + fabsf(v.z));
        v.xy() = (v.z >= 0.0f) ? v.xy() : (vec2(1.0f) - abs(vec2(v.yx()))) * sign(vec2(v.xy()));
        return v.xy();
    }
    vec3 unpackUnitOctahedral(vec2 v) {
        vec3 nor = vec3(v.xy(), 1.0f - fabsf(v.x) - fabsf(v.y));
        nor.xy() = (nor.z >= 0.0f) ? v.xy() : (vec2(1.0f) - abs(vec2(v.yx()))) * sign(vec2(v.xy()));
        return normalize(nor);
    }
}