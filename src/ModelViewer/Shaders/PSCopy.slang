Texture2D srcTexture;
SamplerState sampler;

[shader("fragment")]
float4 fragMain(float4 p: SV_Position) : SV_TARGET {

    return srcTexture.Sample(sampler, p.xy / p.w);
}
