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
BoundingBox BoundingBoxScreenspace(BoundingBox bbBox)
{
    return bbBox.Transform(g_SceneGlobals.camera.viewProjection);    
}
bool FrustumCull(BoundingBox bbBox)
{
    SceneCamera camera = g_SceneGlobals.camera;
    return bbBox.Intersect(camera.clipPlanes[0], camera.clipPlanes[1], camera.clipPlanes[2], camera.clipPlanes[3], camera.clipPlanes[4], camera.clipPlanes[5]) != INTERSECT_VOLUMES_DISJOINT;
}
uint CalculateLOD(BoundingBox bbBoxss)
{
    float2 wh = 2 * bbBoxss.Extents.xy;
    return floor((MAX_LOD_COUNT - 1) * saturate(-0.5 * log10(wh.x * wh.y + EPSILON)));
}
bool OcclusionCull(BoundingBox bbBoxss)
{    
    // Uses depth pyramid from the early pass as a occlusion heuristic
    // Works really well when the camera/scene is mostly static
    // see https://www.rastergrid.com/blog/2010/10/hierarchical-z-map-based-occlusion-culling/
    // see https://github.com/zeux/niagara/blob/master/src/shaders/drawcull.comp.glsl
    Texture2D<float4> depthPyramid = ResourceDescriptorHeap[HIZHandle];
    float maxPixelExtent = max(g_SceneGlobals.frameDimension.x * bbBoxss.Extents.x, g_SceneGlobals.frameDimension.y * bbBoxss.Extents.y) * 2;
    float hizLOD = clamp(floor(log2(maxPixelExtent)), 0, HIZMips);    
    float smpDepth = depthPyramid.SampleLevel(g_HIZSampler, clip2UV(bbBoxss.Center.xy), hizLOD).r; // sample at the center
#ifdef INVERSE_Z        
        float minDepth = bbBoxss.Center.z + bbBoxss.Extents.z; // Center + Extent -> Max. With Inverse Z higher Z value means closer to POV.
        bool occluded = minDepth < smpDepth; // Something is closer to POV than this instance...
#else
    float minDepth = bbBoxss.Center.z - bbBoxss.Extents.z;
    bool occluded = minDepth > smpDepth;
#endif
    return occluded;
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
    BoundingBox bbBox = meshBuffer.boundingBox.Transform(instance.transform);
    SceneCamera camera = g_SceneGlobals.camera;     
    // Only append instances that were visible last frame            
    if (!VISIBLE(index))
        return;
    // Frustum cull
    if (g_SceneGlobals.frustum_cull() && !FrustumCull(bbBox)) // failed!
        return;
    // Assgin LODs and draw
    BoundingBox bbBoxSS = BoundingBoxScreenspace(bbBox);
    uint LOD = CalculateLOD(bbBoxSS);
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
    BoundingBox bbBox = meshBuffer.boundingBox.Transform(instance.transform);
    SceneCamera camera = g_SceneGlobals.camera;
    BoundingBox bbBoxSS = BoundingBoxScreenspace(bbBox);
    // Frustum cull
    if (g_SceneGlobals.frustum_cull() && !FrustumCull(bbBox)) // failed!    
    {
        SET_CULLED(index);
        return;
    }
    // Occlusion cull
    if (g_SceneGlobals.occlusion_cull())
    {
        bool occluded = OcclusionCull(bbBoxSS);        
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
    uint LOD = CalculateLOD(bbBoxSS);
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