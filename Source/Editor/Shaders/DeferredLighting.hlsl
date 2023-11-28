#include "Common.hlsli"

ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
StructuredBuffer<SceneLight> g_Lights : register(t0, space0);
SamplerState g_Sampler : register(s0, space0);
#include "LightingCommon.hlsli"

cbuffer MRTHandles : register(b1, space0)
{
    uint albedoHandle;
    uint normalHandle;
    uint materialHandle;
    uint emissiveHandle;
    uint depthHandle;
    uint frameBufferHandle;
}

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > g_SceneGlobals.frameDimension))
        return;
    
    Texture2D albedoTex = ResourceDescriptorHeap[albedoHandle];
    Texture2D normalTex = ResourceDescriptorHeap[normalHandle];
    Texture2D materialTex = ResourceDescriptorHeap[materialHandle];
    Texture2D emissiveTex = ResourceDescriptorHeap[emissiveHandle];
    Texture2D depthTex = ResourceDescriptorHeap[depthHandle];    
    RWTexture2D<float4> frameBuffer = ResourceDescriptorHeap[frameBufferHandle];
    
    float4 albedoSmp = albedoTex[DTid];
    if (g_SceneGlobals.debug_view_albedo() || g_SceneGlobals.debug_view_lod() /* see GBuffer.hlsl */)
    {
        frameBuffer[DTid] = float4(albedoSmp.rgb, 1);
        return;
    }
    
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
    float Zview = clipZ2ViewZ(Zss, g_SceneGlobals.camera.nearZ, g_SceneGlobals.camera.farZ);
    float3 P = UV2WorldSpace(UV, Zss, g_SceneGlobals.camera.invViewProjection);
    float3 V = normalize(g_SceneGlobals.camera.position.xyz - P);
    if (Zview >= g_SceneGlobals.camera.farZ - 1.0f /*offset to compensate for precision*/) // Discard far plane pixels
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
        if (!light.enabled)
            continue;
        // Spot lights
        float attenuation = 1.0f;
        float3 L = splat3(0); 
        if (light.type == SCENE_LIGHT_TYPE_DIRECTIONAL)
        {
            L = light.direction.xyz;
        }
        else
        {            
            float3 Point2Light = light.position.xyz - P;
            L = normalize(Point2Light);        
            float distance = length(Point2Light);
            float radius = light.radius;
            attenuation = max(min(1.0 - pow(distance / radius, 4.0), 1.0), 0.0) / pow(distance, 2.0);
        }
        float3 k = light.color.rgb * light.intensity * attenuation;
        shade_direct(L, V, N, baseColor, metal, alphaRoughness, k, diffuse, specular);
    }
    if (g_SceneGlobals.probe.enabled)
    {
        shade_indirect(g_SceneGlobals.probe, V, N, baseColor, metal, rough, ao, diffuse, specular);
    }
    float3 finalColor = diffuse + specular + emissiveSmp.rgb;
    frameBuffer[DTid] = float4(finalColor, 1.0f);
}