#include "MVMeshlet.h"
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
[shader("fragment")]
float4 meshletFrag(VertexOutput vtx) : SV_Target {
    uint mhash = hash(vtx.meshlet);
    float3 col = float3(float(mhash & 255), float((mhash >> 8) & 255), float((mhash >> 16) & 255)) / 255.0;
    col *= clamp(normalize(vtx.normal).z,0,1); // L=(0,0,1)
    return float4(col, 1.0);
}
