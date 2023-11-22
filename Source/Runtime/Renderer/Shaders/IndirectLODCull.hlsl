#include "Common.hlsli"
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
StructuredBuffer<SceneMeshInstance> g_SceneMeshInstances : register(t0, space0);
AppendStructuredBuffer<IndirectCommand> g_Commands : register(u0, space0);
RWByteAddressBuffer g_Visibility : register(u1, space0);
SamplerState g_HIZSampler : register(s0, space0);
cbuffer HIZData : register(b1, space0)
{
    uint hizHeapHandle;
    uint hizMips;
}
#define VISIBLE(index) (g_Visibility.Load(index * 4) == 0) // clears set these to 0
#define SET_VISIBLE(index) (g_Visibility.Store(index * 4,0))
#define SET_CULLED(index) (g_Visibility.Store(index * 4,1))

// Each thread of the CS operates on one of the indirect commands
[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main_early(uint index : SV_DispatchThreadID)
{    
    if (index < g_SceneGlobals.numMeshInstances)
    {
        IndirectCommand cmd;
        SceneMeshInstance instance = g_SceneMeshInstances[index];
        SceneCamera camera = g_SceneGlobals.camera;     
        BoundingBox bbBox = instance.boundingBox.Transform(instance.transform);
        BoundingSphere bbSphere = instance.boundingSphere.Transform(instance.transform);
        // Only DRAW instances that were visible last frame
        if (!instance.visible())
            return;
        bool visible = VISIBLE(index);
        if (g_SceneGlobals.occlusion_cull() && instance.occlusion_occludee() && !visible)
            return;
        // Frustum cull
        if (g_SceneGlobals.frustum_cull() && bbBox.Intersect(camera.clipPlanes[0], camera.clipPlanes[1], camera.clipPlanes[2], camera.clipPlanes[3], camera.clipPlanes[4], camera.clipPlanes[5]) == INTERSECT_VOLUMES_DISJOINT)
            return;
        // Assgin LODs and draw
        // xxx seems screen space spread heuristic is prone to LOD pop-ins?
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

[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main_late(uint index : SV_DispatchThreadID)
{        
    if (g_SceneGlobals.occlusion_cull() && index < g_SceneGlobals.numMeshInstances)
    {
        IndirectCommand cmd;
        SceneMeshInstance instance = g_SceneMeshInstances[index];
        SceneCamera camera = g_SceneGlobals.camera;
        BoundingBox bbBox = instance.boundingBox.Transform(instance.transform);
        BoundingSphere bbSphere = instance.boundingSphere.Transform(instance.transform);
        if (!instance.visible())
            return;
        // Frustum cull
        if (g_SceneGlobals.frustum_cull() && bbBox.Intersect(camera.clipPlanes[0], camera.clipPlanes[1], camera.clipPlanes[2], camera.clipPlanes[3], camera.clipPlanes[4], camera.clipPlanes[5]) == INTERSECT_VOLUMES_DISJOINT)
        {
            SET_CULLED(index);
            return;
        }
        // Occlusion cull        
        // Uses depth pyramid from the early pass as a occlusion heuristic
        // Works really well when the camera/scene is mostly static
        // see https://www.rastergrid.com/blog/2010/10/hierarchical-z-map-based-occlusion-culling/
        // see https://github.com/zeux/niagara/blob/master/src/shaders/drawcull.comp.glsl
        Texture2D<float4> hiz = ResourceDescriptorHeap[hizHeapHandle];

        BoundingBox bbBoxss = bbBox.Transform(g_SceneGlobals.camera.viewProjection); // Screen space bounding box
        float4 bbBoxRect = float4((bbBoxss.Center + bbBoxss.Extents).xy, (bbBoxss.Center - bbBoxss.Extents).xy);
        float4 bbBoxRectUV = float4(clip2UV(bbBoxRect.xy), clip2UV(bbBoxRect.zw)); // max, min
        float2 bbBoxRectSize = bbBoxss.Extents.xy * 2 * g_SceneGlobals.frameDimension; // extents are half-widths of the axis
        uint hizLOD = ceil(log2(max(bbBoxRectSize.x, bbBoxRectSize.y)));
        hizLOD = clamp(hizLOD, 0, hizMips);
        float4 depths =
        {
            hiz.SampleLevel(g_HIZSampler, bbBoxRectUV.xy, hizLOD).r, // max
            hiz.SampleLevel(g_HIZSampler, bbBoxRectUV.zw, hizLOD).r, // min
            hiz.SampleLevel(g_HIZSampler, bbBoxRectUV.zy, hizLOD).r, // the other two corners
            hiz.SampleLevel(g_HIZSampler, bbBoxRectUV.xw, hizLOD).r
        };
        float minDepth = bbBoxss.Center.z - bbBoxss.Extents.z;
        float maxDepth = bbBoxss.Center.z + bbBoxss.Extents.z;
        float smpDepth = max(depths.x, max(depths.y, max(depths.z, depths.w)));
        bool occluded = false;
#ifdef INVERSE_Z        
        occluded = smpDepth > maxDepth;
#else
        occluded = smpDepth < minDepth;
#endif
        // Occluded. Reject.
        if (instance.occlusion_occludee() && occluded)
        {
            SET_CULLED(index);
            return;
        }
        // Only DRAW instances that were NOT visible last frame
        bool visible = VISIBLE(index);
        if (visible)
            return;
        // Assgin LODs and draw
        // xxx seems screen space spread heuristic is prone to LOD pop-ins?
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
        // Update visbility
        SET_VISIBLE(index);
    }

}