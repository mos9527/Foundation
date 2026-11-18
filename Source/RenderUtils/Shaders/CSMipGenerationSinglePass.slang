// 0: Average, 1: Min, 2: Max
[[SpecializationConstant]] int reductionMode;
struct PushConstants {
    uint2 extents;
    uint mips;
    uint numWorkGroups;
};
[[vk::push_constant]] PushConstants pc;
SamplerState srcSampler;

Texture2DArray<float4> imgSrc;
RWTexture2DArray<float4> imgDst[12];
globallycoherent RWTexture2DArray<float4> imgDst6;
// -- begin SPD definitions
struct SpdGlobalAtomicBuffer
{
    uint counter[6];
};
globallycoherent RWStructuredBuffer<SpdGlobalAtomicBuffer> spdGlobalAtomic;
#define A_GPU
#define A_HLSL
#define A_HALF
#define SPD_PACKED_ONLY
#define SPD_LINEAR_SAMPLER
// vvv You can thank Slang for these vvv
#define min16float half
#define min16float2 half2
#define min16float3 half3
#define min16float4 half4

#define min16uint uint16_t
#define min16uint2 uint16_t2
#define min16uint3 uint16_t3
#define min16uint4 uint16_t4

#define min16int int16_t
#define min16int2 int16_t2
#define min16int3 int16_t3
#define min16int4 int16_t4
// ^^^
#include "ThirdParty/ffx_a.h"
groupshared AU1 spdCounter;
groupshared AH2 spdIntermediateRG[16][16];
groupshared AH2 spdIntermediateBA[16][16];

AH4 SpdLoadSourceImageH(AF2 tex, AU1 slice)
{
    return imgSrc.SampleLevel(srcSampler, float3((tex + float2(0.5)) / float2(pc.extents), slice), 0.0);
}
AH4 SpdLoadH(ASU2 tex, AU1 slice)
{
    // The final workgroup covers a full 64x64 tile, even for rectangular/small mip 6 views.
    int2 extent = int2(max(pc.extents >> 6u, uint2(1u)));
    return imgDst6[uint3(clamp(tex, int2(0), extent - 1), slice)];
}
void SpdStoreH(ASU2 pix, AH4 outValue, AU1 index, AU1 slice)
{
    // Partial 64x64 source tiles still participate in reductions and the global atomic rendezvous.
    // Discard only their out-of-bounds stores; never early-return those lanes from the dispatch.
    uint2 extent = max(pc.extents >> (index + 1u), uint2(1u));
    if (index >= pc.mips || any(pix < int2(0)) || any(uint2(pix) >= extent))
        return;
    if (index != 5){
        imgDst[index][uint3(pix, slice)] = outValue;
        return;
    }
    imgDst6[uint3(pix, slice)] = outValue;
}
void SpdIncreaseAtomicCounter(AU1 slice)
{
    InterlockedAdd(spdGlobalAtomic[0].counter[slice], 1, spdCounter);
}
AU1 SpdGetAtomicCounter()
{
    return spdCounter;
}
void SpdResetAtomicCounter(AU1 slice)
{
    spdGlobalAtomic[0].counter[slice] = 0;
}
AH4 SpdLoadIntermediateH(AU1 x, AU1 y)
{
    return AH4(
    spdIntermediateRG[x][y].x,
    spdIntermediateRG[x][y].y,
    spdIntermediateBA[x][y].x,
    spdIntermediateBA[x][y].y);
}
void SpdStoreIntermediateH(AU1 x, AU1 y, AH4 value)
{
    spdIntermediateRG[x][y] = value.xy;
    spdIntermediateBA[x][y] = value.zw;
}
AH4 SpdReduce4H(AH4 v0, AH4 v1, AH4 v2, AH4 v3)
{
    if (reductionMode == 1)
        return min(min(v0, v1), min(v2, v3));
    if (reductionMode == 2)
        return max(max(v0, v1), max(v2, v3));
    // HDR radiance can overflow a half-precision sum even when the average is representable.
    return AH4((float4(v0) + float4(v1) + float4(v2) + float4(v3)) * 0.25f);
}
#include "ThirdParty/ffx_spd.h"
// -- end SPD definitions

[shader("compute")]
[numthreads(256, 1, 1)]
void csMain(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex) {
    SpdDownsampleH(
        AU2(WorkGroupId.xy),
        AU1(LocalThreadIndex),
        AU1(pc.mips),
        AU1(pc.numWorkGroups),
        AU1(WorkGroupId.z),
        AU2(0, 0));
}
