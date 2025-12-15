// 0: Average, 1: Min, 2: Max
[[SpecializationConstant]] int reductionMode;
struct PushConstants {
    uint2 srcExtents;
    uint mips;
    uint numWorkGroups;
};
[[vk::push_constant]] PushConstants pc;
SamplerState srcSampler;

Texture2DArray<float4> imgSrc;
// XXX: Renderer doesn't do automatic SRV/UAV Array binding yet.
// Since there's no other usage that may require arrays of which, this is used as a workaround.
RWTexture2DArray<float4> imgDst0;
RWTexture2DArray<float4> imgDst1;
RWTexture2DArray<float4> imgDst2;
RWTexture2DArray<float4> imgDst3;
RWTexture2DArray<float4> imgDst4;
RWTexture2DArray<float4> imgDst5;
globallycoherent RWTexture2DArray<float4> imgDst6;
RWTexture2DArray<float4> imgDst7;
RWTexture2DArray<float4> imgDst8;
RWTexture2DArray<float4> imgDst9;
RWTexture2DArray<float4> imgDst10;
RWTexture2DArray<float4> imgDst11;
RWTexture2DArray<float4> imgDst12;
RWTexture2DArray<float4> imgDstMip(uint index)
{
    switch (index)
    {
        case 0: return imgDst0;
        case 1: return imgDst1;
        case 2: return imgDst2;
        case 3: return imgDst3;
        case 4: return imgDst4;
        case 5: return imgDst5;
        case 6: return imgDst6;
        case 7: return imgDst7;
        case 8: return imgDst8;
        case 9: return imgDst9;
        case 10: return imgDst10;
        case 11: return imgDst11;
        case 12: return imgDst12;
        default: return imgDst0;
    }
}

// -- begin SPD definitions
struct SpdGlobalAtomicBuffer
{
    uint counter[6];
};
globallycoherent RWStructuredBuffer<SpdGlobalAtomicBuffer> spdGlobalAtomic;
#define A_GPU
#define A_HLSL
#define SPD_LINEAR_SAMPLER
#include "ThirdParty/ffx_a.h"
groupshared AU1 spdCounter;
groupshared AF1 spdIntermediateR[16][16];
groupshared AF1 spdIntermediateG[16][16];
groupshared AF1 spdIntermediateB[16][16];
groupshared AF1 spdIntermediateA[16][16];

AF4 SpdLoadSourceImage(AF2 tex, AU1 slice)
{
    return imgSrc.SampleLevel(srcSampler, float3((tex + float2(0.5)) / float2(pc.srcExtents), slice), 0.0);
}
AF4 SpdLoad(ASU2 tex, AU1 slice)
{
    return imgDst6[uint3(tex, slice)];
}
void SpdStore(ASU2 pix, AF4 outValue, AU1 index, AU1 slice)
{
    imgDstMip(index + 1)[uint3(pix, slice)] = outValue;
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
AF4 SpdLoadIntermediate(AU1 x, AU1 y)
{
    return AF4(
    spdIntermediateR[x][y],
    spdIntermediateG[x][y],
    spdIntermediateB[x][y],
    spdIntermediateA[x][y]);
}
void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value)
{
    spdIntermediateR[x][y] = value.x;
    spdIntermediateG[x][y] = value.y;
    spdIntermediateB[x][y] = value.z;
    spdIntermediateA[x][y] = value.w;
}
AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3)
{
    switch (reductionMode){
    case 1:
        return min(min(v0, v1), min(v2, v3));
    case 2:
        return max(max(v0, v1), max(v2, v3));
    }
    return (v0+v1+v2+v3)*0.25;
}
#include "ThirdParty/ffx_spd.h"
// -- end SPD definitions

[shader("compute")]
[numthreads(256, 1, 1)]
void csMain(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex) {
    SpdDownsample(
        AU2(WorkGroupId.xy),
        AU1(LocalThreadIndex),
        AU1(pc.mips),
        AU1(pc.numWorkGroups),
        AU1(WorkGroupId.z),
        AU2(0, 0));
}
