import ICommon;
import IMath;

uniform UBO globalParams;
Texture2D<float4> RT1;
Texture2D<float> depth;
RWTexture2D<float> aoOutput;

struct GTAOPushConstants
{
    float radiusPixels;
    float radiusWorld;
    float intensity;
    float bias;
    uint directionCount;
    uint stepCount;
};

[[vk::push_constant]] GTAOPushConstants pc;

float3 ReconstructWorldPosition(uint2 pixel, float ndcDepth)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(globalParams.fbWidth, globalParams.fbHeight);
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, ndcDepth, 1.0f);
    float4 ws = mul(globalParams.inverseViewProj, ndc);
    return ws.xyz / ws.w;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint2 fullExtent = uint2(uint(globalParams.fbWidth), uint(globalParams.fbHeight));
    uint2 halfExtent = (fullExtent + 1u) / 2u;
    if (tid.x >= halfExtent.x || tid.y >= halfExtent.y)
        return;

    uint2 pixel = min(tid * 2u, fullExtent - 1u);
    float centerDepth = depth.Load(uint3(pixel, 0));
    if (centerDepth <= EPS)
    {
        aoOutput[tid] = 1.0f;
        return;
    }

    float3 p = ReconstructWorldPosition(pixel, centerDepth);
    float3 normalWS = unpackUnitOctahedralSnorm(RT1.Load(uint3(pixel, 0)).xy * 2.0f - 1.0f);
    float noise = uintToFloat(hash(uint3(pixel, globalParams.frameNumber)));

    uint directionCount = clamp(pc.directionCount, 1u, 8u);
    uint stepCount = clamp(pc.stepCount, 1u, 16u);

    float occlusion = 0.0f;
    for (uint d = 0u; d < directionCount; d++)
    {
        float angle = (noise + float(d) / float(directionCount)) * Pi;
        float2 dir = float2(cos(angle), sin(angle));
        for (uint side = 0u; side < 2u; side++)
        {
            float sign = side == 0u ? -1.0f : 1.0f;
            for (uint s = 1u; s <= stepCount; s++)
            {
                float stepT = (float(s) + noise) / float(stepCount);
                int2 samplePixel = int2(pixel) + int2(round(dir * sign * stepT * pc.radiusPixels));
                samplePixel = clamp(samplePixel, int2(0), int2(fullExtent) - 1);

                float sampleDepth = depth.Load(uint3(uint2(samplePixel), 0));
                if (sampleDepth <= EPS)
                    continue;

                float3 sampleP = ReconstructWorldPosition(uint2(samplePixel), sampleDepth);
                float3 delta = sampleP - p;
                float dist2 = dot(delta, delta);
                if (dist2 <= 1e-5f)
                    continue;

                float range = saturate(1.0f - dist2 / (pc.radiusWorld * pc.radiusWorld));
                float facing = saturate(dot(normalWS, delta * rsqrt(dist2)) - pc.bias);
                occlusion += facing * range;
            }
        }
    }

    float sampleCount = float(directionCount * 2u * stepCount);
    aoOutput[tid] = saturate(1.0f - occlusion / sampleCount * pc.intensity);
}
