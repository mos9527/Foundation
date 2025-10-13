Texture2D srcTexture;
SamplerState srcSampler;
[[vk::push_constant]] float blur;

[shader("fragment")]
float4 fragMain(float2 p: TEXCOORD0) : SV_Target
{
    return srcTexture.SampleLevel(srcSampler, p, blur);
}