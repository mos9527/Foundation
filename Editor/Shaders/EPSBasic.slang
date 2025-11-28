#include "EFlags.slangi"
[[SpecializationConstant]] int flags;

struct V2F
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    uint32_t meshlet : ID;
};
//
// Fragment Shader
//
uint hash(uint a)
{
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}
float3 hash3(uint a){
    a = hash(a);
    return float3(float(a & 255), float((a >> 8) & 255), float((a >> 16) & 255)) / 255.0;
}

struct Fragment
{
    float4 color : SV_Target;
};

// https://github.com/shader-slang/slang/issues/4120
import glsl;
[[vk_binding(5,0)]] RWTexture2D<uint32_t> overdraw;

[shader("fragment")]
Fragment main(V2F input, float2 fragCoord : SV_Position /* pixel space */)
{
    Fragment output;
    if (flags & kViewMeshlet)
        output.color = float4(hash3(input.meshlet), 1.0);
    else
        output.color = float4(input.normal, 1.0);
    if (flags & kViewOverdraw)
        imageAtomicAdd(overdraw, int2(fragCoord), 1);
    return output;
}
