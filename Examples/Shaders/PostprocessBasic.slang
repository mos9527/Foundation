import IFlags;

Texture2D<float4> bufferA;
Texture2D<float4> bufferB;
SamplerState sampler;

struct PostprocessPushConstants
{
    uint32_t viewFlags;
    uint32_t accumulatedFrames;
};
[[vk::push_constant]] PostprocessPushConstants pc;

float3 SampleRadiance(float2 uv)
{
    if ((pc.viewFlags & uint32_t(ViewFlagsBits.AOVSampleCount)) != 0u)
    {
        float samples = bufferA.SampleLevel(sampler, uv, 0.0f).a;
        float maxSamples = float(pc.accumulatedFrames);
        float ratio = maxSamples > 0.0f ? saturate(samples / maxSamples) : 0.0f;
        return float3(
            ratio > 0.5f ? (ratio - 0.5f) * 2.0f : 0.0f,
            ratio <= 0.5f ? ratio * 2.0f : 1.0f - (ratio - 0.5f) * 2.0f,
            ratio < 0.5f ? 1.0f - ratio * 2.0f : 0.0f);
    }
    if ((pc.viewFlags & uint32_t(ViewFlagsBits.AOVDiffuse)) != 0u)
        return bufferA.SampleLevel(sampler, uv, 0.0f).xyz;
    if ((pc.viewFlags & uint32_t(ViewFlagsBits.AOVSpecular)) != 0u)
        return bufferB.SampleLevel(sampler, uv, 0.0f).xyz;
    return bufferA.SampleLevel(sampler, uv, 0.0f).xyz + bufferB.SampleLevel(sampler, uv, 0.0f).xyz;
}

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0) : SV_TARGET {
    float3 src = SampleRadiance(uv);
    src = clamp(src, 0.0f, 1.0f);
    src = pow(src, float3(1.0f / 2.2f));
    return float4(src, 1.0f);
}
