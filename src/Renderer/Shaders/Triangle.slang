struct VSInput {
    float16_t3 pos; // fp16

    uint16_t tp; // octa 8+8
    uint32_t np; // normal packed [octa 15+15, bitangent sign 2]

    float16_t2 texCoord; // fp16
};

struct UniformBuffer {
    float4x4 model;
    float4x4 view;
    float4x4 proj;
};
ConstantBuffer<UniformBuffer> ubo;
SamplerState sampler;
Texture2D texture;

struct VSOutput
{
    float4 pos : SV_Position;
    float3 normal;
    float4 tangent; // T_BSign
};

module Math;
__include Math;

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;

    float4x4 mvp = mul(ubo.proj, mul(ubo.view, ubo.model));
    float3 N; float4 T_BSign;
    UnpackTBN(input.np, input.tp, N, T_BSign);
    output.pos = mul(mvp, float4(input.pos, 1.0));
    output.normal = normalize(mul(ubo.model, float4(N, 0.0)).xyz);
    output.tangent = float4(normalize(mul(ubo.model, T_BSign)).xyz, T_BSign.w);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET {
    return float4(dot(vertIn.normal,float3(0,0,1)));
}
