#define A_GPU 1
#define A_HLSL 1
#define A_GPU 1
#define A_HLSL 1
#include "ffx-spd/ffx_a.h"
#include "Common.hlsli"
groupshared AU1 spd_counter; // store current global atomic counter for all threads within the thread group
groupshared AF4 spd_intermediate[16][16]; // intermediate data storage for inter-mip exchange
ConstantBuffer<FFXSpdConstant> g_SpdConstant : register(b2);

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice)
{
    RWTexture2D<float> mip0 = ResourceDescriptorHeap[g_SpdConstant.mipViewHeapIndex[0].x];
    return mip0[p];
}
AF4 SpdLoad(ASU2 p, AU1 slice)
{
    RWTexture2D<float> mip5 = ResourceDescriptorHeap[g_SpdConstant.mipViewHeapIndex[5].x];
    return mip5[p];
} // load from output MIP 5
void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice)
{
    RWTexture2D<float> mipN = ResourceDescriptorHeap[g_SpdConstant.mipViewHeapIndex[mip].x];
    mipN[p] = value;
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
Void SpdResetAtomicCounter(AU1 slice)
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
AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3)
{
    return (v0 + v1 + v2 + v3) * 0.25;
}
// #define SPD_LINEAR_SAMPLER
#include "ffx-spd/ffx_spd.h"
[numthreads(256, 1, 1)]
void main(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    SpdDownsample(AU2(WorkGroupId.xy), AU1(LocalThreadIndex), AU1(g_SpdConstant.numMips), AU1(g_SpdConstant.numWorkGroups), AU1(WorkGroupId.z));
}
