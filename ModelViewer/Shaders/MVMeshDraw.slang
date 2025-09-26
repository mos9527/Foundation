#include "Common.h"

import "Math/Quaternion";
import "Math/Quantization";

struct VSInput {
    float16_t3 pos; // fp16

    uint16_t tp; // octa 8+8
    uint32_t np; // normal packed [octa 15+15, bitangent sign 2]

    float16_t2 texCoord; // fp16
};

struct VSOutput
{
    float4 pos : SV_Position;
    float3 normal;
    float4 tangent; // T_BSign

    float2 uv;
};

[[vk::push_constant]] DrawPushConstant draw;
StructuredBuffer<InstanceData> scInstance;
StructuredBuffer<MeshDrawIndirectCmd> commands;

float4x4 translation(float3 t){
    return float4x4(
        1,0,0,t.x,
        0,1,0,t.y,
        0,0,1,t.z,
        0,0,0,1
    );
}
[shader("vertex")]
VSOutput vertMain(VSInput input, uint32_t drawID : SV_DrawIndex) {
    VSOutput output;

    uint32_t instanceID = commands[drawID].instanceID;
    float3 N;
    float4 T_BSign;
    unpackTBN(input.np, input.tp, N, T_BSign);
    // Apply transforms from instance data
    InstanceData instance = scInstance[instanceID];
    Quaternion q = Quaternion(instance.q);
    float3 pos = input.pos;
    pos = q.rotate(pos);
    pos = pos + instance.t;

    output.pos = mul(draw.viewProj, float4(pos, 1.0));
    output.normal = q.rotate(N);
    output.tangent = float4(q.rotate(T_BSign.xyz), T_BSign.w);
    output.uv = input.texCoord;

    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET {
    return float4(float3(1) * clamp(dot(vertIn.normal, float3(0, 0, 1)), 0, 1), 1.0);
}
