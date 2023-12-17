#include "Common.hlsli"
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
    float3 normal : NORMAL;  
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

PSInput vs_main(VSInput vertex)
{
    PSInput result;
    SceneMeshInstanceData mesh = GetSceneMeshInstance(g_MeshIndex);
    result.position = mul(mul(float4(vertex.position, 1.0f), mesh.transform), g_Scene.camera.viewProjection);
    result.normal = mul(vertex.normal, (float3x3) mesh.transformInvTranspose);
    result.tangent = mul(vertex.tangent, (float3x3) mesh.transformInvTranspose);
    result.uv = vertex.uv;
    return result;
}
struct MRT
{
    float4 TangentFrame : SV_Target0;
    float4 Gradient : SV_Target1;
    float4 Material : SV_Target2;
};
MRT ps_main(PSInput input)
{
    SceneMeshInstanceData mesh = GetSceneMeshInstance(g_MeshIndex);    
    MRT output;
    
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent - dot(input.tangent, N) * N);
    float3 B = cross(N, T);

    output.TangentFrame = PackQuaternion(QuatFrom3x3(float3x3(T, B, N)));
    output.Gradient = float4(ddx_fine(input.uv), ddy_fine(input.uv));
    output.Material = float4(unpackUnorm2x16(mesh.meshIndex), frac(input.uv / 2));
    return output;
}

