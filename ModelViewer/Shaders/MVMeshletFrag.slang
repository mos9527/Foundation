#include "MVMeshlet.h"

[shader("fragment")]
float4 meshletFrag(VertexOutput vtx) : SV_Target {
    return float4(float3(1) * clamp(dot(vtx.normal, float3(0, 0, 1)), 0, 1), 1.0);
}
