#include "Shared.h"
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
cbuffer MRTHandles : register(b1, space0)
{
    uint frameBufferHandle;    
}

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > g_SceneGlobals.frameDimension))
        return;
   
    RWTexture2D<float4> frameBuffer = ResourceDescriptorHeap[frameBufferHandle];
    frameBuffer[DTid] = pow(frameBuffer[DTid], 1 / 2.2);    
}