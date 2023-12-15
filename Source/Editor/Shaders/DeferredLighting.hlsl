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
    float2 ddx = graidentSmp.rg;
    float2 ddy = graidentSmp.ba;
    float2 UV = materialSmp.ba;
    
    SceneMeshInstanceData mesh = GetSceneMeshInstance(meshIndex);
    SceneMaterial material = GetSceneMaterial(mesh.materialIndex);
    
    float3 baseColor = material.albedo.rgb;
    float alpha = material.albedo.a;
    
    if (material.albedoMap != INVALID_HEAP_HANDLE)
    {
        Texture2D albedo = ResourceDescriptorHeap[material.albedoMap];
        float4 smp = albedo.SampleGrad(g_TextureSampler, (float2) DTid / g_Scene.renderDimension, ddx, ddy);
        baseColor = smp.rgb;
        alpha = smp.a;
    }    
    frameBuffer[DTid] = float4(baseColor, 1.0f);
}
