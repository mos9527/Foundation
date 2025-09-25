#include "Unorm.hpp"
namespace Foundation::Math
{
    constexpr float EPS = 1e-6;
    vec2 packUnitOctahedral(vec3 v) {
        v /= vec3(fabsf(v.x) + fabsf(v.y) + fabsf(v.z));
        return (v.z >= EPS) ? v.xy() : (vec2(1.0f) - abs(vec2(v.yx()))) * sign(vec2(v.xy() + EPS));
    }
    vec3 unpackUnitOctahedral(vec2 v) {
        vec3 nor = vec3(v.xy(), 1.0f - fabsf(v.x) - fabsf(v.y));
        vec2 xy = (nor.z >= EPS) ? v.xy() : (vec2(1.0f) - abs(vec2(v.yx()))) * sign(vec2(v.xy() + EPS));
        return normalize(vec3{ xy.x, xy.y ,nor.z });
    }
}