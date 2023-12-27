#include "Common.hlsli"
#define SHADER_CONSTANT_TYPE AreaReduceConstant
#include "Bindless.hlsli"
groupshared uint instances[RENDERER_FULLSCREEN_THREADS * RENDERER_FULLSCREEN_THREADS];

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main_reduce_instance(uint2 DTid : SV_DispatchThreadID, uint gid : SV_GroupIndex)
{    
    RWByteAddressBuffer output = ResourceDescriptorHeap[g_Shader.outBufferUav];
    // clear groupshared mem first
    instances[gid] = 0xffff;
    GroupMemoryBarrierWithGroupSync();
    uint2 offset = g_Shader.position + DTid;
    if (all(offset < g_Shader.position + g_Shader.extent))
    {
        Texture2D tex = ResourceDescriptorHeap[g_Shader.sourceSrv];
        float4 smp = tex[offset];
        uint instance = packUnorm2x16(smp.rg);
        instances[gid] = instance;
    }
    // Sync reads
    GroupMemoryBarrierWithGroupSync();
    if (gid == 0)
    {
        [unroll]
        for (uint i = 0; i < RENDERER_FULLSCREEN_THREADS * RENDERER_FULLSCREEN_THREADS; i++)
        {
            uint instance = instances[i];
            output.Store((instance) * 4, instance);
        }
    }
}