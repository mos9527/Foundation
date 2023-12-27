#include "Common.hlsli"
#define SHADER_CONSTANT_32_8_TYPE MeshSkinningConstant
#include "Bindless.hlsli"

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

[numthreads(EDITOR_SKINNING_THREADS, 1, 1)]
void main(uint DTid : SV_DispatchThreadID)
{
    StructuredBuffer<MeshSkinningTask> g_Tasks = ResourceDescriptorHeap[g_Shader32.tasksSrv];
    MeshSkinningTask task = g_Tasks[g_Shader32.taskIndex];
    if (DTid >= task.srcNumVertices)
        return;
    StructuredBuffer<SkinningVertex> g_InVertices = ResourceDescriptorHeap[task.srcBufferSrv];
    RWStructuredBuffer<StaticVertex> g_OutVertices = ResourceDescriptorHeap[task.destBufferUav];    
    SkinningVertex input = g_InVertices[DTid];
    StaticVertex output = g_OutVertices[DTid];
    output.prevPosition = output.position;
    output.position = input.position;
    output.normal = input.normal;
    output.tangent = input.tangent;
    output.uv = input.uv;
    // Keyshapes / Morph targets
    if (task.numShapeKeys > 0 && task.keyshapeVerticesSrv != INVALID_HEAP_HANDLE)
    {
        StructuredBuffer<uint> g_InKeyShapeOffsets = ResourceDescriptorHeap[task.keyshapeOffsetsSrv];
        StructuredBuffer<float> g_InKeyShapeWeights = ResourceDescriptorHeap[task.keyshapeWeightsSrv];
        StructuredBuffer<StaticVertex> g_InKeyShapes = ResourceDescriptorHeap[task.keyshapeVerticesSrv];
        [loop]
        for (uint i = 0; i < task.numShapeKeys; i++)
        {
            uint offset = g_InKeyShapeOffsets[i];
            float weight = g_InKeyShapeWeights[i];
            StaticVertex vertexOffset = g_InKeyShapes[offset + DTid];
            output.position += vertexOffset.position * weight;
            output.normal += vertexOffset.normal * weight;
            output.tangent += vertexOffset.tangent * weight;
            output.uv += vertexOffset.uv * weight;
        }
    }
    if (task.numBones > 0 && task.boneMatricesSrv != INVALID_HEAP_HANDLE)
    {        
        float4x4 boneMatrix = 0;
        StructuredBuffer<matrix> g_InBoneMatrices = ResourceDescriptorHeap[task.boneMatricesSrv];        
        boneMatrix += g_InBoneMatrices[input.boneIndices.x] * (float) (input.boneWeights.x);
        boneMatrix += g_InBoneMatrices[input.boneIndices.y] * (float) (input.boneWeights.y);
        boneMatrix += g_InBoneMatrices[input.boneIndices.z] * (float) (input.boneWeights.z);
        boneMatrix += g_InBoneMatrices[input.boneIndices.w] * (float) (input.boneWeights.w);
        output.position = mul(float4(output.position, 1), boneMatrix).xyz;
        output.normal = mul(output.normal, (float3x3) boneMatrix);
        output.tangent = mul(output.tangent, (float3x3) boneMatrix);
    }
    g_OutVertices[DTid] = output;
}