struct PushConstant
{
    float time;
    uint32_t binding;
    uint32_t mipReady;
};

[[vk::push_constant]] PushConstant pc;
SamplerState sampler;
[[vk::binding(0, 1)]] // [binding, set]
texture2D textures[];

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0) : SV_Target
{
    return textures[pc.binding].SampleLevel(sampler, uv, pc.mipReady);
}
