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

float3 LoadRadiance(int3 coord)
{
    if ((pc.viewFlags & uint32_t(ViewFlagsBits.AOVSampleCount)) != 0u)
    {
        float samples = bufferA.Load(coord).a;
        float maxSamples = float(pc.accumulatedFrames);
        float ratio = maxSamples > 0.0f ? saturate(samples / maxSamples) : 0.0f;
        return float3(
            ratio > 0.5f ? (ratio - 0.5f) * 2.0f : 0.0f,
            ratio <= 0.5f ? ratio * 2.0f : 1.0f - (ratio - 0.5f) * 2.0f,
            ratio < 0.5f ? 1.0f - ratio * 2.0f : 0.0f);
    }
    if ((pc.viewFlags & uint32_t(ViewFlagsBits.AOVDiffuse)) != 0u)
        return bufferA.Load(coord).xyz;
    if ((pc.viewFlags & uint32_t(ViewFlagsBits.AOVSpecular)) != 0u)
        return bufferB.Load(coord).xyz;
    return bufferA.Load(coord).xyz + bufferB.Load(coord).xyz;
}

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    float3 src = LoadRadiance(int3(int(fragCoord.x), int(fragCoord.y), 0));
    src = clamp(src, 0.0f, 1.0f);
    src = pow(src, float3(1.0f / 2.2f));
    return float4(src, 1.0f);
}
