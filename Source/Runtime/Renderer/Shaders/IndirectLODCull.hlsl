#include "Common.hlsli"
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
StructuredBuffer<SceneMeshInstance> g_SceneMeshInstances : register(t0, space0);
AppendStructuredBuffer<IndirectCommand> g_Commands : register(u0, space0);
// Each thread of the CS operates on one of the indirect commands
[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main( uint index : SV_DispatchThreadID )
{
    if (index < g_SceneGlobals.numMeshInstances)
    {
        IndirectCommand cmd;
        SceneMeshInstance instance = g_SceneMeshInstances[index];
        SceneCamera camera = g_SceneGlobals.camera;     
        BoundingBox bbBox = instance.boundingBox.Transform(instance.transform);
        BoundingSphere bbSphere = instance.boundingSphere.Transform(instance.transform);
        // Frustum cull
        if (bbBox.Intersect(camera.clipPlanes[0], camera.clipPlanes[1], camera.clipPlanes[2], camera.clipPlanes[3], camera.clipPlanes[4], camera.clipPlanes[5]) == INTERSECT_VOLUMES_DISJOINT)
            return;        
        // LOD
        float Rss = sphereScreenSpaceRadius(bbSphere.Center, bbSphere.Radius, camera.position.xyz, camera.fov);
        int lodIndex = max(ceil((MAX_LOD_COUNT - 1) * Rss), instance.lodOverride); // 0 is the most detailed
        lodIndex = clamp(lodIndex, 0, MAX_LOD_COUNT - 1);        
        SceneMeshLod lod = instance.lods[lodIndex];
       
        cmd.MeshIndex = index;
        cmd.LodIndex = lodIndex;
        cmd.IndexBuffer = lod.indices;
        cmd.VertexBuffer = instance.vertices;
        // xxx batching
        cmd.DrawIndexedArguments.BaseVertexLocation = 0;
        cmd.DrawIndexedArguments.IndexCountPerInstance = lod.numIndices;
        cmd.DrawIndexedArguments.InstanceCount = 1;
        cmd.DrawIndexedArguments.StartInstanceLocation = 0;
        cmd.DrawIndexedArguments.StartIndexLocation = 0;
        g_Commands.Append(cmd);
    }

}