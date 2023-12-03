#include "Common.hlsli"
cbuffer IndirectData : register(b0, space0)
{
    uint g_MeshIndex;
    uint g_LodIndex;
};
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b1, space0);
StructuredBuffer<SceneMeshInstanceData> g_SceneMeshInstances : register(t0, space0);
struct VSInput
{
    float3 position : POSITION;
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
    SceneMeshInstanceData mesh = g_SceneMeshInstances[g_MeshIndex];
    result.position = mul(mul(float4(vertex.position, 1.0f), mesh.transform), g_SceneGlobals.camera.viewProjection);
    return result;
}
float4 ps_main(PSInput input) : SV_Target
{
    return float4(1, 1, 1, 1); // We only wrote to depth
}
