static float2 positions[3] = float2[](
    // CCW
    float2(-1,  3),
    float2(3 , -1),
    float2(-1, -1),
);

struct VertexOutput {
    float4 sv_position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertMain(uint vid : SV_VertexID) {
    VertexOutput output;
    output.sv_position = float4(positions[vid], 0.0, 1.0);
    output.uv = 0.5 * (output.sv_position.xy + float2(1.0, 1.0));
    return output;
}
