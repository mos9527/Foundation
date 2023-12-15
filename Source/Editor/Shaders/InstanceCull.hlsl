#include "Common.hlsli"
#define SHADER_CONSTANT_TYPE InstanceCullConstant
#include "Bindless.hlsli"
#define VISIBLE(index) (g_Visibility.Load(index * 4) == 0) // clears set these to 0
#define SET_VISIBLE(index) (g_Visibility.Store(index * 4,0))
#define SET_CULLED(index) (g_Visibility.Store(index * 4,1))
bool FrustumCull(BoundingBox bbBox, matrix model)
{
    SceneCamera camera = g_Scene.camera;
    bbBox = bbBox.Transform(model);
    return bbBox.Intersect(camera.clipPlanes[0], camera.clipPlanes[1], camera.clipPlanes[2], camera.clipPlanes[3], camera.clipPlanes[4], camera.clipPlanes[5]) != INTERSECT_VOLUMES_DISJOINT;
}
uint CalculateLOD(BoundingBox bbBox, matrix model)
{
    SceneCamera camera = g_Scene.camera;
    BoundingSphere sphere;    
    sphere = sphere.FromBox(bbBox);
    sphere = sphere.Transform(model);
    sphere = sphere.Transform(camera.view);
    
    float2 ndcMin, ndcMax;
    if (sphere.ProjectRect(camera.nearZ, camera.projection[0][0], camera.projection[1][1], ndcMin, ndcMax))
    {
        float2 wh = ndcMax - ndcMin;
        return floor((MAX_LOD_COUNT - 1) * saturate(-0.5 * log10(wh.x * wh.y + EPSILON)));        
    }
    else
    {
        return MAX_LOD_COUNT - 1;
    }
}
bool OcclusionCull(BoundingBox bbBox, matrix model)
{    
    // Uses depth pyramid from the early pass as a occlusion heuristic
    // see https://www.rastergrid.com/blog/2010/10/hierarchical-z-map-based-occlusion-culling/
    // see https://github.com/zeux/niagara/blob/master/src/shaders/drawcull.comp.glsl
    SceneCamera camera = g_Scene.camera;
    BoundingSphere sphere;
    sphere = sphere.FromBox(bbBox);
    sphere = sphere.Transform(model);
    sphere = sphere.Transform(camera.view);
    
    Texture2D<float4> depthPyramid = ResourceDescriptorHeap[g_Shader.hizIndex];
    float2 ndcMin, ndcMax;
    if (sphere.ProjectRect(camera.nearZ, camera.projection[0][0], camera.projection[1][1], ndcMin, ndcMax))
    {
        float4 uvRect = saturate(float4(clip2UV(ndcMin), clip2UV(ndcMax))).xwzy; // uvRect.xy -> Top left, uvRect.zw -> Bottom Right
        float width = (uvRect.z - uvRect.x) * g_Scene.renderDimension.x;
        float height = (uvRect.w - uvRect.y) * g_Scene.renderDimension.y;
        float mip = clamp(ceil(log2(max(width, height) / 2.0f)), 0, g_Shader.hizMips - 1);
        float smpDepth = depthPyramid.SampleLevel(g_DepthReduceSampler, (uvRect.xy + uvRect.zw) / 2, mip);
        float sphereDepth = sphere.Center.z - sphere.Radius;
        float viewDepth = clipZ2ViewZ(smpDepth, camera.nearZ, camera.farZ);    
        return viewDepth < sphereDepth;
    }
    else
    {
        return true;
    }        
}
void PushCommand(IndirectCommand cmd, uint instanceFlags)
{
    for (uint i = 0; i < INSTANCE_CULL_MAX_CMDS; i++)
    {
        if (g_Shader.cmds[i].cmdIndex != INVALID_HEAP_HANDLE)
        {
            AppendStructuredBuffer<IndirectCommand> CMD = ResourceDescriptorHeap[g_Shader.cmds[i].cmdIndex];
            if ((instanceFlags & g_Shader.cmds[i].instanceAllowMask) != 0 && (instanceFlags & g_Shader.cmds[i].instanceRejectMask) == 0)
                CMD.Append(cmd);
        }

    }

}
// Each thread of the CS operates on one of the indirect commands
[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main_early(uint index : SV_DispatchThreadID)
{    
    if (index >= GetSceneMeshInstanceCount())
        return;
    RWByteAddressBuffer g_Visibility = ResourceDescriptorHeap[g_Shader.visibilityIndex];
    SceneMeshInstanceData instance = GetSceneMeshInstance(index);
    BoundingBox bbBox = instance.meshBuffer.boundingBox;
    SceneCamera camera = g_Scene.camera;
    // Only append instances that were visible last frame            
    if (!VISIBLE(index))
        return;
    // Frustum cull
    if (!FrustumCull(bbBox, instance.transform)) // failed!
        return;
    // Assgin LODs and draw
    uint LOD = CalculateLOD(bbBox, instance.transform);
    SceneMeshLod lod = instance.meshBuffer.LOD[LOD];
    IndirectCommand cmd;
    cmd.MeshIndex = instance.meshIndex;
    cmd.LodIndex = LOD;
    cmd.IndexBuffer = lod.indices;
    cmd.VertexBuffer = instance.meshBuffer.VB;
    cmd.DrawIndexedArguments.BaseVertexLocation = 0;
    cmd.DrawIndexedArguments.IndexCountPerInstance = lod.numIndices;
    cmd.DrawIndexedArguments.InstanceCount = 1;
    cmd.DrawIndexedArguments.StartInstanceLocation = 0;
    cmd.DrawIndexedArguments.StartIndexLocation = 0;
    PushCommand(cmd, instance.instanceFlags);
}

[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main_late(uint index : SV_DispatchThreadID)
{        
    if (index >= GetSceneMeshInstanceCount())
        return;
    RWByteAddressBuffer g_Visibility = ResourceDescriptorHeap[g_Shader.visibilityIndex];
    SceneMeshInstanceData instance = GetSceneMeshInstance(index);
    BoundingBox bbBox = instance.meshBuffer.boundingBox;
    SceneCamera camera = g_Scene.camera;
    // Frustum cull
    if (!FrustumCull(bbBox, instance.transform)) // failed!    
    {
        SET_CULLED(index);
        return;
    }
    // Occlusion cull
    bool occluded = false;
    if (!(instance.instanceFlags & INSTANCE_FLAG_TRANSPARENT)) // Transparent objects do not serve as occluders
    {
        occluded = OcclusionCull(bbBox, instance.transform);      
    }
    if (occluded)
    {
        SET_CULLED(index);
        return;
    }
    if (VISIBLE(index))
        return;
    uint LOD = CalculateLOD(bbBox, instance.transform);
    SceneMeshLod lod = instance.meshBuffer.LOD[LOD];
    IndirectCommand cmd;
    cmd.MeshIndex = index;
    cmd.LodIndex = LOD;
    cmd.IndexBuffer = lod.indices;
    cmd.VertexBuffer = instance.meshBuffer.VB;
    cmd.DrawIndexedArguments.BaseVertexLocation = 0;
    cmd.DrawIndexedArguments.IndexCountPerInstance = lod.numIndices;
    cmd.DrawIndexedArguments.InstanceCount = 1;
    cmd.DrawIndexedArguments.StartInstanceLocation = 0;
    cmd.DrawIndexedArguments.StartIndexLocation = 0;
    PushCommand(cmd, instance.instanceFlags);
    SET_VISIBLE(index);
}