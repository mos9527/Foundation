Texture2D srcTexture;
SamplerState sampler;

[shader("fragment")]
float4 fragMain(float4 p: SV_Position) : SV_TARGET {

    return float4(p.xy / p.w, 0.0, 1.0);
}
