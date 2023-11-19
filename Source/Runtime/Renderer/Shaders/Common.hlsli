#ifndef COMMON_INCLUDE_HLSL
#define COMMON_INCLUDE_HLSL
#include "Shared.h"
#define DIELECTRIC_SPECULAR 0.08 // Influenced by Specular (which starts at 0.5)
#define M_PI 3.1415926535897932384626433832795
#define EPISILON 0.000001f
#define splat3(X) float3(X,X,X)

float clampedDot(float3 v1, float3 v2)
{
    return saturate(dot(v1, v2));
}
float2 encodeSpheremapNormal(float3 N)
{
    return (normalize(N.xy) * sqrt(N.z * 0.5 + 0.5));
}
float3 decodeSpheremapNormal(float2 G)
{
    float3 N;
    N.z = dot(G.xy, G.xy) * 2 - 1;
    N.xy = normalize(G.xy) * sqrt(1 - N.z * N.z);
    return N;
}
float3 decodeTangetNormalMap(float3 sample, float3 Tv, float3 Nv)
{
    float3 N = normalize(Nv);
    float3 T = normalize(Tv - dot(Tv, N) * N);
    float3 B = cross(N, T);
    float3 tN = sample * 2 - 1;
    N = normalize(tN.x * T + tN.y * B + tN.z * N);
    return N;
}
float ClipZ2ViewZ(float Zss, float zNear, float zFar)
{
#ifdef INVERSE_Z
    return 2.0 * zNear * zFar / (zFar + zNear - (Zss * 2 - 1) * (zNear - zFar));
#endif
    return 2.0 * zNear * zFar / (zFar + zNear - (Zss * 2 - 1) * (zFar - zNear));
}
float2 UV2Clip(float2 UV)
{
    UV.y = 1 - UV.y;
    return UV * 2 - float2(1, 1);
}
float3 UV2XYZ(float2 uv, uint face)
{
    uv = 2 * uv - float2(1, 1);
    uv.y *= -1;
    float3 R;
    switch (face)
    {
        // ** https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-resources-textures-intro
        case 0: // +x
            R = float3(1.0, uv.y, -uv.x);
            break;
        case 1: // -x
            R = float3(-1.0, uv.y, uv.x);
            break;
        case 2: // +y
            R = float3(uv.x, 1.0, -uv.y);
            break;
        case 3: // -y
            R = float3(uv.x, -1.0, uv.y);
            break;
        case 4: // +z
            R = float3(uv.x, uv.y, 1.0);
            break;
        default: // -z
            R = float3(-uv.x, uv.y, -1.0);
            break;
    }
    return R;
}
float3 UV2WorldSpace(float2 UV, float Zss, matrix inverseViewProjection)
{
    float2 clipXY = UV2Clip(UV);
    float4 PositionProj = float4(clipXY, Zss, 1);
    float4 PositionWS = mul(PositionProj, inverseViewProjection);
    PositionWS /= PositionWS.w;
    return PositionWS.xyz;
}
#endif