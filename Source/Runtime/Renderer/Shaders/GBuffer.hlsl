#include "Common.hlsli"

cbuffer IndirectData : register(b0, space0)
{
    uint g_MeshIndex;
    uint g_LodIndex;
};
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b1, space0);
StructuredBuffer<SceneMeshInstance> g_SceneMeshInstances : register(t0, space0);
StructuredBuffer<SceneMaterial> g_Materials : register(t1, space0);
SamplerState g_Sampler : register(s0, space0);
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
    float4 clipPosition : SS_CLIPPOSITION; // Clip space (w/o div by w)
    float4 prevClipPosition : SS_PREVCLIPPOSITION; // Clip space (w/o div by w)
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

PSInput vs_main(VSInput vertex)
{
    PSInput result;
    SceneMeshInstance mesh = g_SceneMeshInstances[g_MeshIndex];
    result.clipPosition = mul(mul(float4(vertex.position, 1.0f), mesh.transform), g_SceneGlobals.camera.viewProjection);
    result.prevClipPosition = mul(mul(float4(vertex.position, 1.0f), mesh.transformPrev), g_SceneGlobals.cameraPrev.viewProjection);
    result.position = result.clipPosition;
    result.normal = mul(vertex.normal, (float3x3) mesh.transformInvTranspose);
    result.tangent = mul(vertex.tangent, (float3x3) mesh.transformInvTranspose);
    result.uv = vertex.uv;
    return result;
}
struct MRT
{
    float4 AlbedoMask : SV_Target0; // i.e. Basecolor
    float4 Normal : SV_Target1; // Spheremeap encoded normal
    float4 Material : SV_Target2; // PBR props & ...
    float4 Emissive : SV_Target3; // Emissive materials
    float4 Velocity : SV_Target4; // Screen space velocity
};
MRT ps_main(PSInput input)
{
    SceneMeshInstance mesh = g_SceneMeshInstances[g_MeshIndex];
    SceneMaterial material = g_Materials[mesh.materialIndex];
    MRT output;
    float2 ssClip = input.clipPosition.xy / input.clipPosition.w;
    float2 ssClipPrev = input.prevClipPosition.xy / input.prevClipPosition.w;
    output.Velocity = float4((ssClip - ssClipPrev), 0, 0);    
    if (g_SceneGlobals.frameFlags & FRAME_FLAG_GBUFFER_ALBEDO_AS_LOD)
    {
        float lod = (float) g_LodIndex / MAX_LOD_COUNT;
        float3 ramp = colorRamp(lod);
        output.AlbedoMask = float4(ramp, 1);
        output.Normal = float4(1,1,1,1);
        output.Emissive = float4(0, 0, 0, 0);
        output.Material = float4(0, 0, 0, 0);        
        return output;
    }
    output.AlbedoMask = material.albedo;
    if (material.albedoMap != INVALID_HEAP_HANDLE)
    {
        Texture2D albedoMap = ResourceDescriptorHeap[material.albedoMap];
        output.AlbedoMask = albedoMap.Sample(g_Sampler, input.uv);
    }
    float3 N = input.normal;
    if (material.normalMap != INVALID_HEAP_HANDLE)
    {
        Texture2D normalMap = ResourceDescriptorHeap[material.normalMap];        
        N = decodeTangetNormalMap(normalMap.Sample(g_Sampler, input.uv).rgb, input.tangent, input.normal);
    }
    output.Normal = float4(encodeSpheremapNormal(normalize(N)), .0f, .0f);
    output.Material = material.pbr;
    if (material.pbrMap != INVALID_HEAP_HANDLE)
    {
        Texture2D pbrMap = ResourceDescriptorHeap[material.pbrMap];
        output.Material = pbrMap.Sample(g_Sampler, input.uv);
    }
    output.Emissive = material.emissive;
    if (material.emissiveMap != INVALID_HEAP_HANDLE)
    {
        Texture2D emissiveMap = ResourceDescriptorHeap[material.emissiveMap];
        output.Emissive = emissiveMap.Sample(g_Sampler, input.uv);
    }
    return output;
}
