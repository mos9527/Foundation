#include "Common.hlsli"
#define SHADER_CONSTANT_TYPE SilhouetteConstants
#include "Bindless.hlsli"

cbuffer IndirectData : register(b2, space0)
{
    uint g_MeshIndex;
    uint g_LodIndex;
};
struct VSInput
{
    float3 position : POSITION;
    float3 prevPosition : PREVPOSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
};
struct PSInput
{
    float4 position : SV_POSITION; // Pixel coordinates
};

PSInput vs_main(VSInput vertex)
{
    PSInput result;
    SceneMeshInstanceData mesh = GetSceneMeshInstance(g_MeshIndex);
    result.position = mul(mul(float4(vertex.position, 1.0f), mesh.transform), g_Scene.camera.viewProjection);
    return result;
}

// We only wrote to depth
// float4 ps_main(PSInput input) : SV_Target
// {
//     return float4(1, 1, 1, 1); 
// }
