#ifndef COMMON_INCLUDE_HLSL
#define COMMON_INCLUDE_HLSL
#include "Math.hlsli"
#include "Shared.h"
#define DIELECTRIC_SPECULAR 0.08 // Influenced by Specular (which starts at 0.5)
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