#include "Common.hlsli"
#define SHADER_CONSTANT_TYPE DepthSampleToTextureConstant
#include "Bindless.hlsli"

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > g_Shader.dimensions))
        return;
    Texture2D<float4> depth = ResourceDescriptorHeap[g_Shader.depthSRVHeapIndex];
    RWTexture2D<float4> hizMip0 = ResourceDescriptorHeap[g_Shader.hizMip0UavHeapIndex];
    hizMip0[DTid] = depth[DTid]; // These two must be of the same mips. Reduction takes places in SPD pass.
}
