#include "Common.hlsli"
cbuffer Constants : register(b0, space0)
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
    uint u4;
    uint u5;
}
SamplerState g_Sampler : register(s0, space0);
// see https://github.com/KhronosGroup/glTF-IBL-Sampler/blob/master/lib/source/shaders/filter.frag
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 6)]
void main_pano2cube(uint3 DTid : SV_DispatchThreadID)
{
    uint dimension = u0, source = u1, cubemap = u2;
    if (any(DTid.xy > uint2(dimension, dimension)))
        return;
    Texture2D<float4> hdriMap = ResourceDescriptorHeap[source];
    RWTexture2DArray<float4> cubemapOut = ResourceDescriptorHeap[cubemap];
    float3 Ray = normalize(UV2XYZ(DTid.xy / float2(dimension, dimension), DTid.z));
    float2 uv = XYZToPanoUV(Ray);
    cubemapOut[DTid.xyz] = hdriMap.SampleLevel(g_Sampler, uv, 0);
}

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 6)]
void main_prefilter(uint3 DTid : SV_DispatchThreadID)
{
    uint dimension = u0, source = u1, dest = u2, mipIndex = u3, cubeIndex = u4, flags = u5;
    
}

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main_lut(uint3 DTid : SV_DispatchThreadID)
{
    uint cube_dimension = u0, lut_dimension = u1, cubemap = u2, lut = u3, flags = u4;
    
}