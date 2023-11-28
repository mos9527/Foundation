#define A_GPU 1
#define A_HLSL 1
#define A_GPU 1
#define A_HLSL 1
#include "ffx-spd/ffx_a.h"
#include "Common.hlsli"
groupshared AU1 spd_counter; // store current global atomic counter for all threads within the thread group
groupshared AF4 spd_intermediate[16][16]; // intermediate data storage for inter-mip exchange
ConstantBuffer<FFXSpdConstant> g_SpdConstant : register(b0);

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice)
{
    if (any(p > g_SpdConstant.dimensions))
        return splat4(0);
    RWTexture2DArray<float4> mip0 = ResourceDescriptorHeap[g_SpdConstant.dstMipHeapIndex[0].x];
    return mip0[uint3(p, slice)];
}
AF4 SpdLoad(ASU2 p, AU1 slice)
{    
    if (any(p > g_SpdConstant.dimensions))
        return splat4(0);
    RWTexture2DArray<float4> mip5 = ResourceDescriptorHeap[g_SpdConstant.dstMipHeapIndex[5 + 1].x];
    return mip5[uint3(p, slice)];
} // load from output MIP 5 (offset by 1 since mip 0 is the source image)
// same for below
void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice)
{
    if (any(p > g_SpdConstant.dimensions))
        return;
    if (mip == 5)
    {
        globallycoherent RWTexture2DArray<float4> mip6 = ResourceDescriptorHeap[g_SpdConstant.dstMipHeapIndex[6].x]; // must be synced
        mip6[uint3(p, slice)] = value;
        return;
    }
    RWTexture2DArray<float4> mipN = ResourceDescriptorHeap[g_SpdConstant.dstMipHeapIndex[mip + 1].x];
    mipN[uint3(p, slice)] = value;
}
void SpdIncreaseAtomicCounter(AU1 slice)
{
    globallycoherent RWBuffer<uint> globalAtomicCounter = ResourceDescriptorHeap[g_SpdConstant.atomicCounterHeapIndex];
    InterlockedAdd(globalAtomicCounter[slice], 1, spd_counter);
}
AU1 SpdGetAtomicCounter()
{
    return spd_counter;
}
void SpdResetAtomicCounter(AU1 slice)
{
    globallycoherent RWBuffer<uint> globalAtomicCounter = ResourceDescriptorHeap[g_SpdConstant.atomicCounterHeapIndex];
    globalAtomicCounter[slice] = 0;
}
AF4 SpdLoadIntermediate(AU1 x, AU1 y)
{
    return spd_intermediate[x][y];
}
void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value)
{
    spd_intermediate[x][y] = value;
}
// Compiler defined reduction function
#ifndef SPD_REDUCTION_FUNCTION
#define SPD_REDUCTION_FUNCTION (v0+v1+v2+v3)*0.25f
#endif
AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3)
{
    return SPD_REDUCTION_FUNCTION;
}
// #define SPD_LINEAR_SAMPLER
#include "ffx-spd/ffx_spd.h"
[numthreads(256, 1, 1)]
void main(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    SpdDownsample(AU2(WorkGroupId.xy), AU1(LocalThreadIndex), AU1(g_SpdConstant.numMips), AU1(g_SpdConstant.numWorkGroups), AU1(WorkGroupId.z));
}
