#include "Common.hlsli"
#define SHADER_CONSTANT_TYPE ShadingConstants
#include "Bindless.hlsli"
#include "Shading.hlsli"
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
    float4 positionWS : POSITIONWS; // World space    
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

PSInput vs_main(VSInput vertex)
{
    PSInput result;
    SceneMeshInstanceData mesh = GetSceneMeshInstance(g_MeshIndex);
    result.positionWS = mul(float4(vertex.position, 1.0f), mesh.transform);
    result.position = mul(result.positionWS, g_Scene.camera.viewProjection);
    result.normal = mul(vertex.normal, (float3x3) mesh.transformInvTranspose);
    result.tangent = mul(vertex.tangent, (float3x3) mesh.transformInvTranspose);
    result.uv = vertex.uv;
    return result;
}
// Weighted Blended Order-Independent Transparency
// see https://jcgt.org/published/0002/02/09/paper.pdf
// z : viewspace depth
// a : shaded pixel alpha
// This is eq.9 from the paper.
// https://www.desmos.com/calculator/uzkaf95o4f
float w(float z, float a)
{
    return a * max(0.01, min(0.03 / (10e-5 + pow(z / 200.0f, 4)), 3000.0f));
}
struct MRT
{
    float4 Accumalation : SV_Target0; // Blend: sum of Ci.rgb,Ci.a
    float4 Revealage : SV_Target1; // Blend: prod of 1 - Ci.a
};
MRT ps_main(PSInput input)
{
    SceneMeshInstanceData mesh = GetSceneMeshInstance(g_MeshIndex);
    SceneMaterial material = GetSceneMaterial(mesh.materialIndex);
    MRT output;    
    float4 albedo = material.albedo;
    if (material.albedoMap != INVALID_HEAP_HANDLE)
    {
        Texture2D albedoMap = ResourceDescriptorHeap[material.albedoMap];
        albedo = albedoMap.Sample(g_TextureSampler, input.uv);
    }
    float alpha = albedo.a; // xxx GLTF stores alpha in basecolor map
    float3 N = input.normal;
    if (material.normalMap != INVALID_HEAP_HANDLE)
    {
        Texture2D normalMap = ResourceDescriptorHeap[material.normalMap];
        N = decodeTangetNormalMap(normalMap.Sample(g_TextureSampler, input.uv).rgb, input.tangent, input.normal);
    }    
    float4 PBR = material.pbr;
    if (material.pbrMap != INVALID_HEAP_HANDLE)
    {
        Texture2D pbrMap = ResourceDescriptorHeap[material.pbrMap];
        PBR = pbrMap.Sample(g_TextureSampler, input.uv);
    }
    float4 emissive = material.emissive;
    if (material.emissiveMap != INVALID_HEAP_HANDLE)
    {
        Texture2D emissiveMap = ResourceDescriptorHeap[material.emissiveMap];
        emissive = emissiveMap.Sample(g_TextureSampler, input.uv);
    }
    float3 P = input.positionWS.xyz;
    float viewDepth = input.position.w;
    float3 V = normalize(g_Scene.camera.position.xyz - P);
    N = normalize(N);
    float3 baseColor = albedo.rgb;
    float ao = PBR.r;
    float rough = PBR.g;
    float metal = PBR.b;
    float alphaRoughness = rough * rough;
    
    float3 finalColor = splat3(0);
    float3 diffuse = float3(0, 0, 0);
    float3 specular = float3(0, 0, 0);
    [unroll(MAX_LIGHT_COUNT)]
    for (uint i = 0; i < g_Scene.lights.count; i++)
    {
        SceneLight light = GetSceneLight(i);
        if (!light.enabled)
            continue;
        shade_direct(light, P, V, N, baseColor, metal, alphaRoughness, diffuse, specular);
    }
    if (g_Shader.iblProbe.enabled)
    {
        shade_indirect(g_Shader.iblProbe, V, N, baseColor, metal, rough, ao, diffuse, specular);
    }
    finalColor = diffuse + specular + emissive.rgb;
    float weight = w(viewDepth, alpha);
    output.Accumalation = float4(finalColor * weight, alpha * weight);
    output.Revealage = float4(alpha, 0, 0, 0);
    return output;
}
// Only writes the instance ID
// Also, blending is disabled on this pass
float4 ps_main_material(PSInput input) : SV_Target
{
    float4 output = float4(unpackUnorm2x16(g_MeshIndex),input.uv);
    return output;
}
