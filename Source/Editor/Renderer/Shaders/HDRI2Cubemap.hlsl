#include "Common.hlsli"
cbuffer Constants : register(b0, space0)
{
    uint HDRIsrv;
    uint cubemapUAV;
    uint width;
    uint height;
}
SamplerState g_PanoSampler : register(s0, space0);
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 6)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy > uint2(width, height)))
        return;
    Texture2D<float4> hdriMap = ResourceDescriptorHeap[HDRIsrv];
    RWTexture2DArray<float4> cubemap = ResourceDescriptorHeap[cubemapUAV];
    float3 Ray = UV2XYZ(DTid.xy / float2(width, height), DTid.z);
    float2 uv = XYZToPanoUV(Ray);
    cubemap[DTid.xyz] = hdriMap.SampleLevel(g_PanoSampler, uv, 0);
}
