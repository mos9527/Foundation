// 0: Average, 1: Min, 2: Max
[[SpecializationConstant]] int reductionMode;
struct PushConstants {
    uint2 extents;
    uint mips;
    uint numWorkGroups;
    uint sameSrcDst;
};
[[vk::push_constant]] PushConstants pc;
SamplerState srcSampler;

Texture2DArray<float4> imgSrc;
// XXX: Renderer doesn't do automatic SRV/UAV Array binding yet.
// Since there's no other usage that may require arrays of which, this is used as a workaround.
RWTexture2DArray<float4> imgDst[];
globallycoherent RWTexture2DArray<float4> imgDst6;
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
    return imgSrc.SampleLevel(srcSampler, float3((tex + float2(0.5)) / float2(pc.extents), slice), 0.0);
}
AF4 SpdLoad(ASU2 tex, AU1 slice)
{
    return imgDst6[uint3(tex, slice)];
}
void SpdStore(ASU2 pix, AF4 outValue, AU1 index, AU1 slice)
{
    if (index != 5)
        imgDst[index + 1][uint3(pix, slice)] = outValue;
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
    // Needs a copy for MIP 0. We work on 64x64 tiles
    if (!pc.sameSrcDst)
    {
        uint2 tile = WorkGroupId.xy;
        uint2 tileOffset = tile * 64;
        uint2 threadOffset = uint2(LocalThreadIndex % 16, LocalThreadIndex / 16) * 4;
        uint2 pixelCoord = tileOffset + threadOffset;
        for (uint y = 0; y < 4; ++y)
        for (uint x = 0; x < 4; ++x)
        {
            uint2 coord = pixelCoord + uint2(x, y);
            if (coord.x < pc.extents.x && coord.y < pc.extents.y)
            {
                float4 value = SpdLoadSourceImage(AF2(coord), AU1(WorkGroupId.z));
                imgDst[0][uint3(coord, 0)] = value;
            }
        }
    }
}
