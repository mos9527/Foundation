#include "Shared.h"
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
StructuredBuffer<SceneMeshInstance> g_SceneMeshInstances : register(t0, space0);
struct IndirectCommand
{
    D3D12_VERTEX_BUFFER_VIEW VertexBuffer;
    D3D12_INDEX_BUFFER_VIEW IndexBuffer;
    D3D12_DRAW_INDEXED_ARGUMENTS DrawIndexedArguments;
};
AppendStructuredBuffer<IndirectCommand> g_Commands : register(u0, space0);
// Each thread of the CS operates on one of the indirect commands
[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main( uint index : SV_DispatchThreadID )
{
    if (index < g_SceneGlobals.numMeshInstances)
    {
        IndirectCommand cmd;
        SceneMeshLod lod = g_SceneMeshInstances[index].lods[0];
        cmd.IndexBuffer = lod.indices;
        cmd.VertexBuffer = g_SceneMeshInstances[index].vertices;
        cmd.DrawIndexedArguments.BaseVertexLocation = 0;
        cmd.DrawIndexedArguments.IndexCountPerInstance = lod.numIndices;
        cmd.DrawIndexedArguments.InstanceCount = 1;
        cmd.DrawIndexedArguments.StartInstanceLocation = 0;
        cmd.DrawIndexedArguments.StartIndexLocation = 0;
        g_Commands.Append(cmd);
    }

}