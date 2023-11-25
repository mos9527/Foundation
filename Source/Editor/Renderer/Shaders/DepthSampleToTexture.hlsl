#include "Common.hlsli"
ConstantBuffer<DepthSampleToTextureConstant> g_SampleParams : register(b0, space0);
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > g_SampleParams.dimensions))
        return;
    Texture2D<float4> depth = ResourceDescriptorHeap[g_SampleParams.depthSRVHeapIndex];
    RWTexture2D<float4> hizMip0 = ResourceDescriptorHeap[g_SampleParams.hizMip0UavHeapIndex];
    hizMip0[DTid] = depth[DTid]; // These two must be of the same mips. Reduction takes places in SPD pass.
}