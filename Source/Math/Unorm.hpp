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
    vec2 packUnitOctahedral(vec3 v);
    vec3 unpackUnitOctahedral(vec2 v);
}
