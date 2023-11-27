#include "Common.hlsli"
cbuffer Constants : register(b0, space0)
{
    uint HDRIsrv;
    uint cubemapUAV;
    uint width;
    uint height;
}
SamplerState g_PanoSampler : register(s0, space0);
// see https://github.com/KhronosGroup/glTF-IBL-Sampler/blob/master/lib/source/shaders/filter.frag
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 6)]
void main_pano2cube(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy > uint2(width, height)))
        return;
    Texture2D<float4> hdriMap = ResourceDescriptorHeap[HDRIsrv];
    RWTexture2DArray<float4> cubemap = ResourceDescriptorHeap[cubemapUAV];
    float3 Ray = normalize(UV2XYZ(DTid.xy / float2(width, height), DTid.z));
    float2 uv = XYZToPanoUV(Ray);
    cubemap[DTid.xyz] = hdriMap.SampleLevel(g_PanoSampler, uv, 0);
}
