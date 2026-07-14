import ICommon;
import IMath;

uniform UBO globalParams;
Texture2D<float> depth;
RWTexture2D<float2> motionVectors;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint2 extent = uint2(uint(globalParams.fbWidth), uint(globalParams.fbHeight));
    if (any(tid >= extent))
        return;

    float2 currentUV = (float2(tid) + 0.5f) / float2(extent);
    float reverseZ = depth.Load(uint3(tid, 0));
    float4 currentNDC = float4(currentUV.x * 2.0f - 1.0f, 1.0f - currentUV.y * 2.0f, reverseZ, 1.0f);
    float4 world = mul(globalParams.inverseViewProj, currentNDC);
    if (reverseZ <= EPS)
    {
        float4 rayPoint = mul(globalParams.inverseViewProj, float4(currentNDC.xy, 1.0f, 1.0f));
        float3 direction = normalize(rayPoint.xyz / rayPoint.w - globalParams.camPosition.xyz);
        world = float4(globalParams.camPosition.xyz + direction * 1e6f, 1.0f);
    }
    else
    {
        world /= world.w;
    }

    float4 previousClip = mul(globalParams.previousViewProj, world);
    float2 previousNDC = previousClip.xy / max(abs(previousClip.w), 1e-8f);
    float2 previousUV = float2(previousNDC.x * 0.5f + 0.5f, 0.5f - previousNDC.y * 0.5f);
    motionVectors[tid] = currentUV - previousUV;
}
