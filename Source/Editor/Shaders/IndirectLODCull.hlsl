#include "Common.hlsli"
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
StructuredBuffer<SceneMeshInstance> g_SceneMeshInstances : register(t0, space0);
RWByteAddressBuffer g_Visibility : register(u0, space0);
SamplerState g_HIZSampler : register(s0, space0);
cbuffer CMDHeapHandles : register(b1, space0)
{
    uint indirectCMDHeapHandle;
    uint transparencyIndirectCMDHeapHandle;
    uint silhouetteIndirectCMDHeapHandle;
}
cbuffer HIZData : register(b2, space0)
{
    uint hizHeapHandle;
    uint hizMips;
}
// see https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-advanced-stages-cs-resources#byte-address-buffer
// TL;DR: ...uses a byte value offset from the beginning of the buffer to access data. 
//        The byte value must be a multiple of four so that it is DWORD aligned. If any other value is provided, behavior is undefined
#define VISIBLE(index) (g_Visibility.Load(index * 4) == 0) // clears set these to 0
#define SET_VISIBLE(index) (g_Visibility.Store(index * 4,0))
#define SET_CULLED(index) (g_Visibility.Store(index * 4,1))

void screenspaceBoundingBox(BoundingBox bbBox, out BoundingBox bbBoxss, out float4 bbBoxRectUV, out uint hizLOD, out uint LOD)
{
    bbBoxss = bbBox.Transform(g_SceneGlobals.camera.viewProjection); // Screen space bounding box
    float2 bbMinSS = (bbBoxss.Center - bbBoxss.Extents).xy;
    float2 bbMaxSS = (bbBoxss.Center + bbBoxss.Extents).xy;
    bbBoxRectUV = float4(saturate(clip2UV(bbMaxSS)), saturate(clip2UV(bbMinSS))); // max, min
    float2 bbBoxRectSize = 2 * bbBoxss.Extents.xy * g_SceneGlobals.frameDimension; // extents are half-widths of the axis
    hizLOD = floor(log2(max(bbBoxRectSize.x, bbBoxRectSize.y)));    
    float bbArea = bbBoxss.Extents.x * bbBoxss.Extents.y * 4;
    LOD = (MAX_LOD_COUNT - 1) * clamp(-0.5 * log2(bbArea), 0, 1);
}

// Each thread of the CS operates on one of the indirect commands
[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main_early(uint index : SV_DispatchThreadID)
{    
    if (index < g_SceneGlobals.numMeshInstances)
    {
        SceneMeshInstance instance = g_SceneMeshInstances[index];
        bool inv = instance.instanceFlags & INSTANCE_FLAG_INVISIBLE;
        if (inv || instance.has_transparency() || instance.silhouette()) // Handle silhouette/transparency in the late pass
            return;
        SceneCamera camera = g_SceneGlobals.camera;     
        BoundingBox bbBox = instance.boundingBox.Transform(instance.transform);
        // Only DRAW instances that were visible last frame        
        bool visible = VISIBLE(index);
        if (!visible)
            return;
        // Frustum cull
        if (g_SceneGlobals.frustum_cull() && bbBox.Intersect(camera.clipPlanes[0], camera.clipPlanes[1], camera.clipPlanes[2], camera.clipPlanes[3], camera.clipPlanes[4], camera.clipPlanes[5]) == INTERSECT_VOLUMES_DISJOINT)
            return;
        // Assgin LODs and draw
        // xxx seems screen space spread heuristic is prone to LOD pop-ins?
        BoundingBox bbBoxss;
        float4 bbBoxRectUV;
        uint hizLOD, lodIndex;
        screenspaceBoundingBox(bbBox, bbBoxss, bbBoxRectUV, hizLOD, lodIndex);
        SceneMeshLod lod = instance.lods[lodIndex];
        IndirectCommand cmd;
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
        AppendStructuredBuffer<IndirectCommand> commands = ResourceDescriptorHeap[indirectCMDHeapHandle];
        commands.Append(cmd);
    }
}

[numthreads(RENDERER_INSTANCE_CULL_THREADS, 1, 1)]
void main_late(uint index : SV_DispatchThreadID)
{        
    if (index < g_SceneGlobals.numMeshInstances)
    {
        SceneMeshInstance instance = g_SceneMeshInstances[index];
        if (instance.invisible())
            return;        
        SceneCamera camera = g_SceneGlobals.camera;
        BoundingBox bbBox = instance.boundingBox.Transform(instance.transform);
        BoundingBox bbBoxss;
        float4 bbBoxRectUV;
        uint hizLOD, lodIndex;
        screenspaceBoundingBox(bbBox, bbBoxss, bbBoxRectUV, hizLOD, lodIndex);
        // Frustum cull
        if (g_SceneGlobals.frustum_cull() && bbBox.Intersect(camera.clipPlanes[0], camera.clipPlanes[1], camera.clipPlanes[2], camera.clipPlanes[3], camera.clipPlanes[4], camera.clipPlanes[5]) == INTERSECT_VOLUMES_DISJOINT)
        {
            SET_CULLED(index);
            return;
        }
        if (g_SceneGlobals.occlusion_cull())
        {
        // Occlusion cull        
        // Uses depth pyramid from the early pass as a occlusion heuristic
        // Works really well when the camera/scene is mostly static
        // see https://www.rastergrid.com/blog/2010/10/hierarchical-z-map-based-occlusion-culling/
        // see https://github.com/zeux/niagara/blob/master/src/shaders/drawcull.comp.glsl
            Texture2D<float4> hiz = ResourceDescriptorHeap[hizHeapHandle];
            float2 smpCoord = (bbBoxRectUV.xy + bbBoxRectUV.zw) * 0.5f;
            float smpDepth = hiz.SampleLevel(g_HIZSampler, smpCoord, hizLOD).r; // sample at the centre
#ifdef INVERSE_Z        
        float minDepth = bbBoxss.Center.z + bbBoxss.Extents.z; // Center + Extent -> Max. With Inverse Z higher Z value means closer to POV.
        bool occluded = minDepth < smpDepth; // Something is closer to POV than this instance...
#else
        float minDepth = bbBoxss.Center.z - bbBoxss.Extents.z;
        bool occluded = minDepth > smpDepth;
#endif
        // Meaning it's Occluded. Reject.
            if (instance.occlusion_occludee() && occluded)
            {
            SET_CULLED(index);
                return;
            }
        }
        if (!instance.has_transparency() && !instance.silhouette())
        {
            // Only DRAW opaque instances that were NOT visible last frame
            bool visible = VISIBLE(index);
            if (visible)
                return;            
        } 
        // Assgin LODs and draw
        // At this stage the instance is either Opaque, passed all tests, but not drawn (queued) in the first pass
        // or Transparent, passed frustum cull. These are only drawn (queued) here.
        // As transparency objects needs another pass to be drawn POST opaque draw & shade.
        // This LOD though...
        // xxx seems screen space spread heuristic is prone to LOD pop-ins?        
        SceneMeshLod lod = instance.lods[lodIndex];
        IndirectCommand cmd;
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
        if (instance.has_transparency())
        {
            AppendStructuredBuffer<IndirectCommand> transparencyCommands = ResourceDescriptorHeap[transparencyIndirectCMDHeapHandle];
            transparencyCommands.Append(cmd);            
        }
        else
        {
            AppendStructuredBuffer<IndirectCommand> commands = ResourceDescriptorHeap[indirectCMDHeapHandle];
            commands.Append(cmd);            
        }
        if (instance.silhouette())
        {
            AppendStructuredBuffer<IndirectCommand> silhouetteCommands = ResourceDescriptorHeap[silhouetteIndirectCMDHeapHandle];
            silhouetteCommands.Append(cmd);
        }
        // Update visbility
        SET_VISIBLE(index);
    }

}