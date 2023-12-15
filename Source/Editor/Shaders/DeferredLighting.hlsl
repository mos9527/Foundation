#include "Common.hlsli"
#define SHADER_CONSTANT_TYPE ShadingConstants
#define SHADER_CONSTANT_32_8_TYPE DeferredShadingConstants
#include "Bindless.hlsli"
#include "Shading.hlsli"

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > g_Scene.renderDimension))
        return;
    
    Texture2D tangentFrameTex = ResourceDescriptorHeap[g_Shader32.tangentFrameSrv];
    Texture2D gradientTex = ResourceDescriptorHeap[g_Shader32.gradientSrv];
    Texture2D materialTex = ResourceDescriptorHeap[g_Shader32.materialSrv];    
    Texture2D depthTex = ResourceDescriptorHeap[g_Shader32.depthSrv];    
    RWTexture2D<float4> frameBuffer = ResourceDescriptorHeap[g_Shader32.framebufferUav];
    
    float4 tangentFrameSmp = tangentFrameTex[DTid];
    float4 graidentSmp = gradientTex[DTid];
    float4 materialSmp = materialTex[DTid];
    float4 depthSmp = depthTex[DTid];
    
    uint meshIndex = packSnorm2x16(materialSmp.rg);
    float3x3 TBN = QuatTo3x3(UnpackQuaternion(tangentFrameSmp));
    float3 T = TBN._11_21_31;
    float3 B = TBN._12_22_32;
    float3 N = TBN._13_23_33;
    
    float2 ddx = graidentSmp.rg;
    float2 ddy = graidentSmp.ba;
    float2 UV = materialSmp.ba;
    
    SceneMeshInstanceData mesh = GetSceneMeshInstance(meshIndex);
    SceneMaterial material = GetSceneMaterial(mesh.materialIndex);
    
    float3 baseColor = material.albedo.rgb;
    float alpha = material.albedo.a;
    
    if (material.albedoMap != INVALID_HEAP_HANDLE)
    {
        Texture2D tex = ResourceDescriptorHeap[material.albedoMap];
        float4 smp = tex.SampleGrad(g_TextureSampler, (float2) DTid / g_Scene.renderDimension, ddx, ddy);
        baseColor = smp.rgb;
        alpha = smp.a;
    }
    if (material.normalMap != INVALID_HEAP_HANDLE)
    {
        Texture2D tex = ResourceDescriptorHeap[material.normalMap];
        float4 smp = tex.SampleGrad(g_TextureSampler, (float2) DTid / g_Scene.renderDimension, ddx, ddy);
        N = decodeTangetNormalMap(smp.rgb, T, N);
    }
    float3 pbr = material.pbr;
    if (material.pbrMap != INVALID_HEAP_HANDLE)
    {
        Texture2D tex = ResourceDescriptorHeap[material.pbrMap];
        float4 smp = tex.SampleGrad(g_TextureSampler, (float2) DTid / g_Scene.renderDimension, ddx, ddy);
        pbr = smp.rgb;
    }
    float3 emissive = material.emissive;
    if (material.emissiveMap != INVALID_HEAP_HANDLE)
    {
        Texture2D tex = ResourceDescriptorHeap[material.emissiveMap];
        float4 smp = tex.SampleGrad(g_TextureSampler, (float2) DTid / g_Scene.renderDimension, ddx, ddy);
        emissive = smp.rgb;
    }
    float ao = pbr.r;
    float rough = pbr.g;
    float metal = pbr.b;
    float alphaRoughness = rough * rough;
    
    float2 Pss = DTid / float2(g_Scene.renderDimension);
    float Zss = depthSmp.r;
    float Zview = clipZ2ViewZ(Zss, g_Scene.camera.nearZ, g_Scene.camera.farZ);
    float3 P = UV2WorldSpace(UV, Zss, g_Scene.camera.invViewProjection);
    float3 V = normalize(g_Scene.camera.position.xyz - P);
    if (Zview >= g_Scene.camera.farZ - 1.0f /*offset to compensate for precision*/) // Discard far plane pixels
    {
        frameBuffer[DTid] = float4(0, 0, 0, 0);
        return;
    }
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
    float3 finalColor = diffuse + specular + emissive;
    frameBuffer[DTid] = float4(finalColor, 1.0f);
}
