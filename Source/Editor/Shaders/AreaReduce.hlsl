#include "Common.hlsli"
cbuffer Constants : register(b0, space0)
{
    uint g_X0;
    uint g_Y0;
    uint g_Width;
    uint g_Height;
    uint g_Input;
    uint g_Output;
}
groupshared uint instances[RENDERER_FULLSCREEN_THREADS * RENDERER_FULLSCREEN_THREADS];

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main_reduce_instance(uint2 DTid : SV_DispatchThreadID, uint gid : SV_GroupIndex)
{    
    uint2 offset = uint2(g_X0, g_Y0) + DTid;    
    if (any(offset >= uint2(g_X0 + g_Width, g_Y0 + g_Height)))
        return;
    Texture2D tex = ResourceDescriptorHeap[g_Input];
    RWByteAddressBuffer output = ResourceDescriptorHeap[g_Output];
    float4 smp = tex[offset];
    instances[gid] = unpackUnorm8to16(smp.ba);
    // Sync reads
    GroupMemoryBarrierWithGroupSync();
    if (gid == 0)
    {
        [unroll]
        for (uint i = 0; i < RENDERER_FULLSCREEN_THREADS * RENDERER_FULLSCREEN_THREADS; i++)
        {
            uint instace = instances[i];
            if (instace >= 1) // Instance ID + 1 is stored. Clear otherwise.
            {
                instace--;
                output.Store(instace * 4, 1); // Mark in-range
            }
        }
    }

}