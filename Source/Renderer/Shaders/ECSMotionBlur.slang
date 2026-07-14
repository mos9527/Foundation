import ICommon;
import IMath;

uniform UBO globalParams;
SamplerState colorSampler;
Texture2D<float4> diffuseInput;
Texture2D<float4> specularInput;
Texture2D<float2> motionVectors;
Texture2D<float> depth;
RWTexture2D<float4> diffuseOutput;
RWTexture2D<float4> specularOutput;

struct MotionBlurPushConstants
{
    float intensity;
    uint sampleCount;
    float maximumVelocity;
    float minimumVelocity;
    float depthComparisonExtent;
};

[[vk::push_constant]] MotionBlurPushConstants pc;

float LinearEyeDepth(float reverseZ)
{
    // Reverse-Z: near ≈ 1, far ≈ 0. Convert to positive eye-space distance.
    return globalParams.zNear / max(reverseZ, 1e-6f);
}

float DepthWeight(float centerEyeZ, float sampleEyeZ)
{
    // From Unity HDRP
    return saturate(0.5f + pc.depthComparisonExtent * (sampleEyeZ - centerEyeZ));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint2 extent = uint2(uint(globalParams.fbWidth), uint(globalParams.fbHeight));
    if (tid.x >= extent.x || tid.y >= extent.y)
        return;

    float4 centerDiffuse = diffuseInput.Load(uint3(tid, 0));
    float4 centerSpecular = specularInput.Load(uint3(tid, 0));
    float centerDepth = depth.Load(uint3(tid, 0));

    float2 mvUV = motionVectors.Load(uint3(tid, 0));
    float2 screenSize = float2(extent);
    float2 mvPx = mvUV * screenSize * pc.intensity;
    float mvLen = length(mvPx);
    if (mvLen < pc.minimumVelocity)
    {
        diffuseOutput[tid] = centerDiffuse;
        specularOutput[tid] = centerSpecular;
        return;
    }
    // Clamp to maximumVelocity (pixels), then gather along ±half the shutter.
    mvPx *= min(1.0f, pc.maximumVelocity / mvLen);
    float2 dirUV = (mvPx * 0.5f) / screenSize;

    uint samples = max(pc.sampleCount, 2u);
    if ((samples & 1u) != 0u)
        samples += 1u; // keep even so taps split evenly about the center
    uint halfSamples = samples / 2u;

    bool centerBackground = centerDepth <= EPS;
    float centerEyeZ = centerBackground ? 0.0f : LinearEyeDepth(centerDepth);
    float2 centerUV = (float2(tid) + 0.5f) / screenSize;

    float4 sumDiffuse = centerDiffuse;
    float4 sumSpecular = centerSpecular;
    float weightSum = 1.0f;

    for (uint i = 1u; i <= halfSamples; ++i)
    {
        float t = float(i) / float(halfSamples);
        float2 offset = dirUV * t;

        float2 uv0 = saturate(centerUV + offset);
        float2 uv1 = saturate(centerUV - offset);
        uint2 p0 = uint2(uv0 * screenSize);
        uint2 p1 = uint2(uv1 * screenSize);
        p0 = min(p0, extent - 1u);
        p1 = min(p1, extent - 1u);

        float d0 = depth.Load(uint3(p0, 0));
        float d1 = depth.Load(uint3(p1, 0));
        float w0 = centerBackground ? (d0 <= EPS ? 1.0f : 0.0f)
                                    : ((d0 > EPS) ? DepthWeight(centerEyeZ, LinearEyeDepth(d0)) : 0.0f);
        float w1 = centerBackground ? (d1 <= EPS ? 1.0f : 0.0f)
                                    : ((d1 > EPS) ? DepthWeight(centerEyeZ, LinearEyeDepth(d1)) : 0.0f);

        sumDiffuse += diffuseInput.SampleLevel(colorSampler, uv0, 0) * w0;
        sumDiffuse += diffuseInput.SampleLevel(colorSampler, uv1, 0) * w1;
        sumSpecular += specularInput.SampleLevel(colorSampler, uv0, 0) * w0;
        sumSpecular += specularInput.SampleLevel(colorSampler, uv1, 0) * w1;
        weightSum += w0 + w1;
    }

    float invW = 1.0f / max(weightSum, 1e-4f);
    diffuseOutput[tid] = sumDiffuse * invW;
    specularOutput[tid] = sumSpecular * invW;
}
