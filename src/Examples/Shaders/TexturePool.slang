[[vk::push_constant]] float time;
SamplerState sampler;
[[vk::binding(0, 1)]] // [set, binding]
texture2D textures[];

[shader("fragment")]
float4 fragMain(float2 p: TEXCOORD0) : SV_Target
{
    return textures[0].Sample(sampler,  p);
}
