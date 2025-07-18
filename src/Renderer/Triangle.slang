
struct VSInput {
    float2 inPosition;
    float3 inColor;
};

struct UniformBuffer {
    float4x4 model;
    float4x4 view;
    float4x4 proj;
};
ConstantBuffer<UniformBuffer> ubo;

struct VSOutput
{
    float4 pos : SV_Position;
    float3 color;
};

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;

    float4x4 MVP = ubo.proj * ubo.view * ubo.model;
    output.pos = mul(MVP, float4(input.inPosition, 0.0, 1.0));
    output.color = input.inColor;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET {
    return float4(vertIn.color, 1.0);
}
