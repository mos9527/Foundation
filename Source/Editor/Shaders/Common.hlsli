#ifndef COMMON_INCLUDE_HLSL
#define COMMON_INCLUDE_HLSL
#ifndef SHARED_DEFINES
#define SHARED_DEFINES
#include "Defines.hpp"
#endif
#include "Math.hlsli"
#include "Shared.h"

float3 colorRamp(float v)
{
    static const float3 RGBcolors[] =
    {
        { 0, 0, 0 },
        { 0, 0, 1 },
        { 0, 1, 0 },
        { 0, 1, 1 },
        { 1, 0, 0 },
        { 1, 0, 1 },
        { 1, 1, 0 },
        { 1, 1, 1 }
    };
    return lerp(RGBcolors[floor(v * 7)], RGBcolors[ceil(v * 7)], v);
}
#endif