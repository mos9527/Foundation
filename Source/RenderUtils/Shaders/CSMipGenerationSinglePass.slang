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
    return imgDst6[uint3(tex, slice)];
}
void SpdStoreH(ASU2 pix, AH4 outValue, AU1 index, AU1 slice)
{
    if (index != 5)
        imgDst[index][uint3(pix, slice)] = outValue;
   else
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
    return (v0+v1+v2+v3)*half(0.25);
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
