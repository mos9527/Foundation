#include "ICommon.slang"

uniform UBO globalParams;
RaytracingAccelerationStructure AS;
[[vk_binding(0,1)]] RWTexture2D<float4> output;

[shader("raygeneration")]
void RayGen()
{
    uint2 pix = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 uv = float2(pix) / dim;
    float4 ndcPosition = float4(uv, 1.0f, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);
    float3 p = wsPosition.xyz / wsPosition.w;
    float3 eye = globalParams.camPosition;
    float3 dir = normalize(p - eye);
    output[pix] = float4(dir * 0.5 + 0.5, 1.0);
}
