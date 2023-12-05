#include "Common.hlsli"
// see MeshAsset.hpp
struct StaticVertex
{
    float3 position;
    float3 prevPosition;
    float3 normal;
    float3 tangent;
    float2 uv;
};
struct SkinningVertex
{
    float3 position;
    float3 normal;
    float3 tangent;
    float2 uv;
    uint4 boneIndices;
    float4 boneWeights;
};
cbuffer Constant : register(b0, space0)
{
    uint g_MeshIndex;
    uint g_PassIndex;
};
RWStructuredBuffer<StaticVertex> g_OutVertices : register(u0, space0);
RWStructuredBuffer<SceneMeshBuffer> g_OutMeshBuffer : register(u1, space0);
StructuredBuffer<SkinningConstants> g_SkinningConstants : register(t0, space0);
StructuredBuffer<SkinningVertex> g_InVertices : register(t1, space0);
StructuredBuffer<matrix> g_InBoneMatrices : register(t2, space0);
StructuredBuffer<float> g_InKeyShapeWeights : register(t3, space0);
StructuredBuffer<StaticVertex> g_InKeyShapes : register(t4, space0);
StructuredBuffer<uint> g_InKeyShapeOffsets : register(t5, space0);
RWByteAddressBuffer g_ReductionBuffer : register(u2, space0);

[numthreads(EDITOR_SKINNING_THREADS, 1, 1)]
void main_skinning(uint DTid : SV_DispatchThreadID)
{
    SkinningConstants constants = g_SkinningConstants[g_MeshIndex];
    if (DTid >= constants.numVertices)
        return;
    SkinningVertex input = g_InVertices[DTid];
    StaticVertex output = g_OutVertices[DTid];
    output.prevPosition = output.position;
    output.position = input.position;
    output.normal = input.normal;
    output.tangent = input.tangent;
    output.uv = input.uv;
    for (uint i = 0; i < constants.numShapeKeys; i++)
    {
        uint offset = g_InKeyShapeOffsets[i];
        StaticVertex vertexOffset = g_InKeyShapes[offset + DTid];
        output.position += vertexOffset.position * g_InKeyShapeWeights[i];
        output.normal += vertexOffset.normal * g_InKeyShapeWeights[i];
        output.tangent += vertexOffset.tangent * g_InKeyShapeWeights[i];
        output.uv += vertexOffset.uv * g_InKeyShapeWeights[i];
    }
    if (constants.numBones > 0)
    {
        float4x4 boneMatrix = 0;
        boneMatrix += g_InBoneMatrices[input.boneIndices.x] * (float)(input.boneWeights.x);
        boneMatrix += g_InBoneMatrices[input.boneIndices.y] * (float)(input.boneWeights.y);
        boneMatrix += g_InBoneMatrices[input.boneIndices.z] * (float)(input.boneWeights.z);
        boneMatrix += g_InBoneMatrices[input.boneIndices.w] * (float)(input.boneWeights.w);
        output.position = mul(float4(output.position, 1), boneMatrix).xyz;
        output.normal = mul(output.normal, (float3x3) boneMatrix);
        output.tangent = mul(output.tangent, (float3x3) boneMatrix);
    }
    g_OutVertices[DTid] = output;
}
// see https://www.jeremyong.com/graphics/2023/09/05/f32-interlocked-min-max-hlsl/
uint asuint_signed(float fvalue)
{        
    // Perform a bitwise cast of the float value to an unsigned value.
    uint uvalue = asuint(fvalue);
    if ((uvalue >> 31) == 0)
    {
        // The sign bit wasn't set, so set it temporarily.
        return uvalue | (1 << 31);
    }
    else
    {
        // In the case where we started with a negative value, take
        // the ones complement.
        return ~uvalue;
    }   
}
float asfloat_signed(uint uvalue)
{
    if ((uvalue >> 31) == 0)
    {
        // The MSB is unset, so take the complement, then bitcast,
        // turning this back into a negative floating point value.
        return asfloat(~uvalue);
    }
    else
    {
        // The MSB is set, so we started with a positive float.
        // Unset the MSB and bitcast.
        return asfloat(uvalue & ~(1u << 31));
    }
}
// Dispatch(1,1,1)
void main_reduce_early(uint DTid : SV_DispatchThreadID, uint gid : SV_GroupIndex)
// Reset buffers
{
    if (DTid == 0)
    {        
        g_ReductionBuffer.Store(4 * (g_MeshIndex + 0), asuint_signed(+1e10));
        g_ReductionBuffer.Store(4 * (g_MeshIndex + 1), asuint_signed(+1e10));
        g_ReductionBuffer.Store(4 * (g_MeshIndex + 2), asuint_signed(+1e10));
        g_ReductionBuffer.Store(4 * (g_MeshIndex + 3), asuint_signed(-1e10));
        g_ReductionBuffer.Store(4 * (g_MeshIndex + 4), asuint_signed(-1e10));
        g_ReductionBuffer.Store(4 * (g_MeshIndex + 5), asuint_signed(-1e10));
    }
}

groupshared float3 groupPositions[EDITOR_SKINNING_THREADS];
// Dispatch(DivUp(Verts,Threads),1,1)
void main_reduce_mid(uint DTid : SV_DispatchThreadID, uint gid : SV_GroupIndex)
// Reduce min-max
{   
    StaticVertex vtx = g_OutVertices[DTid];
    groupPositions[gid] = vtx.position;
    if (gid == 0)
    {
        float3 pMin = groupPositions[0];
        float3 pMax = groupPositions[0];
        // Do minmax reduction on group thread 0
        [unroll]
        for (uint i = 0; i < EDITOR_SKINNING_THREADS; i++)
        {
            pMin = min(pMin, groupPositions[i]);
            pMax = max(pMax, groupPositions[i]);
            // xxx Accomplish this with Wave Intrisics
        }
        GroupMemoryBarrierWithGroupSync();
        // Do minmax reduction globally        
        g_ReductionBuffer.InterlockedMin(4 * (g_MeshIndex + 0), asuint_signed(pMin.x));
        g_ReductionBuffer.InterlockedMin(4 * (g_MeshIndex + 1), asuint_signed(pMin.y));
        g_ReductionBuffer.InterlockedMin(4 * (g_MeshIndex + 2), asuint_signed(pMin.z));
        g_ReductionBuffer.InterlockedMax(4 * (g_MeshIndex + 3), asuint_signed(pMax.x));
        g_ReductionBuffer.InterlockedMax(4 * (g_MeshIndex + 4), asuint_signed(pMax.y));
        g_ReductionBuffer.InterlockedMax(4 * (g_MeshIndex + 5), asuint_signed(pMax.z));
    }
}

// Dispatch(1,1,1)
void main_reduce_late(uint DTid : SV_DispatchThreadID, uint gid : SV_GroupIndex)
// Write back to MeshBuffer
{
    if (DTid == 0)
    {
        SceneMeshBuffer MB;
        SkinningConstants constants = g_SkinningConstants[g_MeshIndex];
        float3 pMin, pMax;
        pMin.x = asfloat_signed(g_ReductionBuffer.Load(4 * (g_MeshIndex + 0)));
        pMin.y = asfloat_signed(g_ReductionBuffer.Load(4 * (g_MeshIndex + 1)));
        pMin.z = asfloat_signed(g_ReductionBuffer.Load(4 * (g_MeshIndex + 2)));
        pMax.x = asfloat_signed(g_ReductionBuffer.Load(4 * (g_MeshIndex + 3)));
        pMax.y = asfloat_signed(g_ReductionBuffer.Load(4 * (g_MeshIndex + 4)));
        pMax.z = asfloat_signed(g_ReductionBuffer.Load(4 * (g_MeshIndex + 5)));
        MB.VB = constants.VB;
        MB.IB = constants.IB;
        MB.boundingBox.Center = (pMin + pMax) / 2;
        MB.boundingBox.Extents = (pMax - pMin) / 2;
        g_OutMeshBuffer[g_MeshIndex] = MB;
    }
}

[numthreads(EDITOR_SKINNING_THREADS, 1, 1)]
void main_reduce(uint DTid : SV_DispatchThreadID, uint gid : SV_GroupIndex)
{
    if (g_PassIndex == 0)
        main_reduce_early(DTid, gid);
    if (g_PassIndex == 1)
        main_reduce_mid(DTid, gid);
    if (g_PassIndex == 2)
        main_reduce_late(DTid, gid);
}