#include "Shared.h"
#include "Common.hlsli"
#include "BRDF.hlsli"

ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
cbuffer MRTHandles : register(b1, space0)
{
    uint albedoHandle;
    uint normalHandle;
    uint materialHandle;
    uint emissiveHandle;
    uint depthHandle;
    uint frameBufferHandle;
}
StructuredBuffer<SceneLight> g_Lights : register(t0, space0);

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > g_SceneGlobals.frameDimension)) // Does this crash the shader??
        return;
    
    Texture2D albedoTex = ResourceDescriptorHeap[albedoHandle];
    Texture2D normalTex = ResourceDescriptorHeap[normalHandle];
    Texture2D materialTex = ResourceDescriptorHeap[materialHandle];
    Texture2D emissiveTex = ResourceDescriptorHeap[emissiveHandle];
    Texture2D depthTex = ResourceDescriptorHeap[depthHandle];    
    RWTexture2D<float4> frameBuffer = ResourceDescriptorHeap[frameBufferHandle];
    
    float4 albedoSmp = albedoTex[DTid];
    float4 normalSmp = normalTex[DTid];
    float4 materialSmp = materialTex[DTid];
    float4 emissiveSmp = emissiveTex[DTid];
    float4 depthSmp = depthTex[DTid];
    
    float3 PBR = materialSmp.rgb;
    
    float3 baseColor = albedoSmp.rgb;
    float ao = PBR.r;
    float rough = PBR.g;
    float metal = PBR.b;
    float alphaRoughness = rough * rough;
    
    float3 N = decodeSpheremapNormal(normalSmp.rg);
    float2 UV = DTid / float2(g_SceneGlobals.frameDimension);
    
    float Zss = depthSmp.r;
    float Zview = ClipZ2ViewZ(Zss, g_SceneGlobals.camera.nearZ, g_SceneGlobals.camera.farZ);
    float3 P = UV2WorldSpace(UV, Zss, g_SceneGlobals.camera.invViewProjection);
    float3 V = normalize(g_SceneGlobals.camera.position.xyz - P);
    
    if (Zview >= g_SceneGlobals.camera.farZ) // Discard far plane pixels
    {
        frameBuffer[DTid] = float4(0, 0, 0, 0);
        return;
    }
    float3 diffuse = float3(0, 0, 0);
    float3 specular = float3(0, 0, 0);
    [unroll(MAX_LIGHT_COUNT)]
    for (uint i = 0; i < g_SceneGlobals.numLights; i++)
    {
        SceneLight light = g_Lights[i];
        // Spot lights
        float3 Point2Light = light.position.xyz - P;
        float distance = length(Point2Light);
        float radius = light.radius;
        float attenuation = max(min(1.0 - pow(distance / radius, 4.0), 1.0), 0.0) / pow(distance, 2.0);
        
        float3 L = normalize(Point2Light);
        float3 H = normalize(L + V);
    
        float NoL = clampedDot(N, L);
        float VoH = clampedDot(V, H);
        float NoH = clampedDot(N, H);
        float NoV = clampedDot(N, V);
        
        float3 F0 = lerp(0.5f * splat3(DIELECTRIC_SPECULAR), baseColor, metal);
        float3 c_diffuse = lerp(baseColor, splat3(0), metal);
        float3 k = light.color.rgb * light.intensity * attenuation;
        diffuse += k * NoL * BRDF_Lambertian(c_diffuse);
        specular += k * NoL * BRDF_GGX_Specular(F0, splat3(1), VoH, NoL, NoV, NoH, alphaRoughness);
    }
    float3 finalColor = diffuse + specular + emissiveSmp.rgb;
    frameBuffer[DTid] = float4(finalColor, 1.0f);
}