#include "Shared.h"
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
cbuffer MRTHandles : register(b1, space0)
{
    uint albedoHandle;
    uint normalHandle;
    uint materialHandle;
    uint depthHandle;
    uint frameBufferHandle;
}

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{    
    if (any(DTid > g_SceneGlobals.frameDimension)) // Does this crash the shader??
        return;
    
    Texture2D albedoTex = ResourceDescriptorHeap[albedoHandle];
    Texture2D normalTex = ResourceDescriptorHeap[normalHandle];
    Texture2D materialTex = ResourceDescriptorHeap[materialHandle];
    Texture2D depthTex = ResourceDescriptorHeap[depthHandle];
    
    RWTexture2D<float4> frameBuffer = ResourceDescriptorHeap[frameBufferHandle];
    
    float4 albedo = albedoTex[DTid];
    float4 normalSmp = normalTex[DTid];
    float4 material = materialTex[DTid];
    float4 depthSmp = depthTex[DTid];
    
    frameBuffer[DTid] = albedo.rgba;
}