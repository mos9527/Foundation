/**
 * Hello Triangle Example
 */
static float2 positions[3] = float2[](
    float2(-0.5, 0.5),
    float2(0.5, 0.5),
    float2(0.0, -0.5),
);

static float3 colors[3] = float3[](
    float3(1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0),
    float3(0.0, 0.0, 1.0)
);
static float phase[3] = float[](
    0.0,
    0.33,
    0.66
);
struct VertexOutput {
    float3 color;
    float4 sv_position : SV_Position;
};

[[vk::push_constant]] float time;

[shader("vertex")]
VertexOutput vertMain(uint vid : SV_VertexID) {
    VertexOutput output;
    output.sv_position = float4(positions[vid], 0.0, 1.0) + float4(sin(phase[vid] + time), cos(phase[vid] + time), 0, 0) * 0.5;
    output.color = colors[vid] + float3(sin(phase[vid] + time), cos(phase[vid] + time), sin(phase[vid] + time)) * 0.5;
    return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput inVert) : SV_Target
{
    float3 color = inVert.color * (0.5 + abs(sin(time)) * 0.5);
    return float4(color, 1.0);
}
