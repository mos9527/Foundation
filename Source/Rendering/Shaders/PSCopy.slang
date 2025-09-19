Texture2D srcTexture;
SamplerState sampler;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0) : SV_TARGET {

    return srcTexture.Sample(sampler, uv);
}
