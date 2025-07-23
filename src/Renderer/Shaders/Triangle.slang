struct VSInput {
    float2 inPosition;
    float3 inColor;
    float2 inTexCoord;
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
    float2 fragTexCoord;
    float3 vertColor;
};

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;

    float4x4 mvp = mul(ubo.proj, mul(ubo.view, ubo.model));
    output.pos = mul(mvp, float4(input.inPosition, 0.0, 1.0));
    output.fragTexCoord = input.inTexCoord;
    output.vertColor = input.inColor;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET {
    return texture.Sample(sampler, vertIn.fragTexCoord) * float4(vertIn.vertColor, 1.0);
}
