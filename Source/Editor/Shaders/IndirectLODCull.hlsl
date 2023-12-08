#include "Common.hlsli"
StructuredBuffer<SceneMeshInstanceData> g_SceneMeshInstanceData : register(t0, space0);
StructuredBuffer<SceneMeshBuffer> g_StaticMeshBuffers : register(t1, space0);
StructuredBuffer<SceneMeshBuffer> g_SkinnedMeshBuffers : register(t2, space0);
SamplerState g_HIZSampler : register(s0, space0);
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
cbuffer CMDHeapHandles : register(b1, space0)
{
    uint GBufferCMD;
    uint TransparencyCMD;
    uint SilhouetteCMD;
}
cbuffer HIZData : register(b2, space0)
{
    uint HIZHandle;
    uint HIZMips;
}
RWByteAddressBuffer g_Visibility : register(u0, space0);
// see https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-advanced-stages-cs-resources#byte-address-buffer
// TL;DR: ...uses a byte value offset from the beginning of the buffer to access data. 
//        The byte value must be a multiple of four so that it is DWORD aligned. If any other value is provided, behavior is undefined
#define VISIBLE(index) (g_Visibility.Load(index * 4) == 0) // clears set these to 0
#define SET_VISIBLE(index) (g_Visibility.Store(index * 4,0))
#define SET_CULLED(index) (g_Visibility.Store(index * 4,1))
bool FrustumCull(BoundingBox bbBox, matrix model)
{
    SceneCamera camera = g_SceneGlobals.camera;
    bbBox = bbBox.Transform(model);
    return bbBox.Intersect(camera.clipPlanes[0], camera.clipPlanes[1], camera.clipPlanes[2], camera.clipPlanes[3], camera.clipPlanes[4], camera.clipPlanes[5]) != INTERSECT_VOLUMES_DISJOINT;
}
uint CalculateLOD(BoundingBox bbBox, matrix model)
{
    SceneCamera camera = g_SceneGlobals.camera;
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
    // Works really well when the camera/scene is mostly static
    // see https://www.rastergrid.com/blog/2010/10/hierarchical-z-map-based-occlusion-culling/
    // see https://github.com/zeux/niagara/blob/master/src/shaders/drawcull.comp.glsl
    SceneCamera camera = g_SceneGlobals.camera;
    BoundingSphere sphere;
    sphere = sphere.FromBox(bbBox);
    sphere = sphere.Transform(model);
    sphere = sphere.Transform(camera.view);
    
    Texture2D<float4> depthPyramid = ResourceDescriptorHeap[HIZHandle];
    float2 ndcMin, ndcMax;
    if (sphere.ProjectRect(camera.nearZ, camera.projection[0][0], camera.projection[1][1], ndcMin, ndcMax))
    {
        float4 uvRect = saturate(float4(clip2UV(ndcMin), clip2UV(ndcMax))).xwzy; // uvRect.xy -> Top left, uvRect.zw -> Bottom Right
        float width = (uvRect.z - uvRect.x) * g_SceneGlobals.frameDimension.x;
        float height = (uvRect.w - uvRect.y) * g_SceneGlobals.frameDimension.y;        
        float mip = clamp(ceil(log2(max(width, height) / 2.0f)), 0, HIZMips - 1);
        float smpDepth = depthPyramid.SampleLevel(g_HIZSampler, (uvRect.xy + uvRect.zw) / 2, mip);
        float sphereDepth = sphere.Center.z - sphere.Radius;
        float viewDepth = clipZ2ViewZ(smpDepth, camera.nearZ, camera.farZ);    
        return viewDepth < sphereDepth;
    }
    else
    {
        return true;
    }        
}
SceneMeshBuffer GetMeshBuffer(SceneMeshInstanceData instance)
{
    if (instance.instanceMeshType == INSTANCE_MESH_TYPE_STATIC)
    {
        return g_StaticMeshBuffers[instance.instanceMeshLocalIndex];
    }
    if (instance.instanceMeshType == INSTANCE_MESH_TYPE_SKINNED)
    {
        return g_SkinnedMeshBuffers[instance.instanceMeshLocalIndex];
    }
    else
    {
        // This should never happen
        return g_StaticMeshBuffers[instance.instanceMeshLocalIndex];
    }
}
// Each thread of the CS operates on one of the indirect commands
[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main_early(uint index : SV_DispatchThreadID)
{    
    if (index >= g_SceneGlobals.numMeshInstances)
        return;
    SceneMeshInstanceData instance = g_SceneMeshInstanceData[index];
    if (instance.invisible() || instance.has_transparency() || instance.silhouette()) // Handle silhouette/transparency in the late pass
        return;
    SceneMeshBuffer meshBuffer = GetMeshBuffer(instance);
    BoundingBox bbBox = meshBuffer.boundingBox;
    SceneCamera camera = g_SceneGlobals.camera;     
    // Only append instances that were visible last frame            
    if (!VISIBLE(index))
        return;
    // Frustum cull
    if (g_SceneGlobals.frustum_cull() && !FrustumCull(bbBox, instance.transform)) // failed!
        return;
    // Assgin LODs and draw
    uint LOD = CalculateLOD(bbBox, instance.transform);
    SceneMeshLod lod = meshBuffer.IB[LOD];
    IndirectCommand cmd;
    cmd.MeshIndex = instance.instanceMeshGlobalIndex;
    cmd.LodIndex = LOD;
    cmd.IndexBuffer = lod.indices;
    cmd.VertexBuffer = meshBuffer.VB;
    cmd.DrawIndexedArguments.BaseVertexLocation = 0;
    cmd.DrawIndexedArguments.IndexCountPerInstance = lod.numIndices;
    cmd.DrawIndexedArguments.InstanceCount = 1;
    cmd.DrawIndexedArguments.StartInstanceLocation = 0;
    cmd.DrawIndexedArguments.StartIndexLocation = 0;
    AppendStructuredBuffer<IndirectCommand> CMD = ResourceDescriptorHeap[GBufferCMD];
    CMD.Append(cmd);    
}

[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main_late(uint index : SV_DispatchThreadID)
{        
    if (index >= g_SceneGlobals.numMeshInstances)
        return;
    SceneMeshInstanceData instance = g_SceneMeshInstanceData[index];
    if (instance.invisible())
        return;        
    SceneMeshBuffer meshBuffer = GetMeshBuffer(instance);
    BoundingBox bbBox = meshBuffer.boundingBox;
    SceneCamera camera = g_SceneGlobals.camera;
    // Frustum cull
    if (g_SceneGlobals.frustum_cull() && !FrustumCull(bbBox, instance.transform)) // failed!    
    {
        SET_CULLED(index);
        return;
    }
    // Occlusion cull
    if (g_SceneGlobals.occlusion_cull())
    {
        bool occluded = OcclusionCull(bbBox, instance.transform);
        if (occluded)
        {
            SET_CULLED(index);
            return;
        }
    }
    if (!instance.has_transparency() && !instance.silhouette())
    {
        // Only append opaque (gbuffer) instances that were NOT visible last frame
        if (VISIBLE(index))
            return;            
    }   
    uint LOD = CalculateLOD(bbBox, instance.transform);
    SceneMeshLod lod = meshBuffer.IB[LOD];    
    IndirectCommand cmd;
    cmd.MeshIndex = index;
    cmd.LodIndex = LOD;
    cmd.IndexBuffer = lod.indices;
    cmd.VertexBuffer = meshBuffer.VB;   
    cmd.DrawIndexedArguments.BaseVertexLocation = 0;
    cmd.DrawIndexedArguments.IndexCountPerInstance = lod.numIndices;
    cmd.DrawIndexedArguments.InstanceCount = 1;
    cmd.DrawIndexedArguments.StartInstanceLocation = 0;
    cmd.DrawIndexedArguments.StartIndexLocation = 0;
    // Decide where the command should go
    if (instance.has_transparency()) // Transparency rendering
    {
        AppendStructuredBuffer<IndirectCommand> CMD = ResourceDescriptorHeap[TransparencyCMD];
        CMD.Append(cmd);
    }
    else // GBuffer
    {
        AppendStructuredBuffer<IndirectCommand> CMD = ResourceDescriptorHeap[GBufferCMD];
        CMD.Append(cmd);
    }
    if (instance.silhouette()) // Additoally, Silhouette/Outliner
    {
        AppendStructuredBuffer<IndirectCommand> CMD = ResourceDescriptorHeap[SilhouetteCMD];
        CMD.Append(cmd);
    }    
    SET_VISIBLE(index);
}