// AOV combine
Texture2D<float4> bufferA;
Texture2D<float4> bufferB;
SamplerState sampler;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    float3 src = bufferA.Sample(sampler, uv).xyz + bufferB.Sample(sampler, uv).xyz;
    src = clamp(src, 0.0f, 1.0f);
    src = pow(src, float3(1.0f / 2.2f));
    return float4(src, 1.0f);
}
