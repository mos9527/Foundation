#ifndef SHARED_INCLUDE
#define SHARED_INCLUDE
#ifdef __cplusplus
#include "../../../Common/Defines.hpp"
#include "../../../pch.hpp"
#pragma pack(push, 4) // otherwise it's 8 on 64-bit systems
#else
#include "Defines.hpp"
// DirectX Types
typedef uint2 D3D12_GPU_VIRTUAL_ADDRESS;
struct D3D12_DRAW_INDEXED_ARGUMENTS
{
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    uint BaseVertexLocation;
    uint StartInstanceLocation;
};
struct D3D12_VERTEX_BUFFER_VIEW
{
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
    uint SizeInBytes;
    uint StrideInBytes;
};
struct D3D12_INDEX_BUFFER_VIEW
{
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
    uint SizeInBytes;
    uint Format;
};
#endif
struct IndirectCommand
{
    uint MeshIndex; // 1
    uint LodIndex; // 1
    D3D12_VERTEX_BUFFER_VIEW VertexBuffer; // 4
    D3D12_INDEX_BUFFER_VIEW IndexBuffer; // 4
    D3D12_DRAW_INDEXED_ARGUMENTS DrawIndexedArguments; // 5
};
struct SceneCamera // ! align for CB
{
    float4 position; // 16

    float fov;
    float aspect;
    float nearZ;
    float farZ; // 16
	
    Plane clipPlanes[6];

    matrix view; 
    matrix projection;
    matrix viewProjection;

    matrix invView;
    matrix invProjection;
    matrix invViewProjection;
};
struct SceneGlobals // ! align for CB
{
    SceneCamera camera;
    SceneCamera cameraPrev; // previous frame

    uint numMeshInstances;
    uint numLights;
    uint2 _pad;

    uint frameFlags;
    uint frameIndex;
    uint2 frameDimension;

    uint sceneVersion;
    uint backBufferIndex;
    uint2 _pad2;

    float frameTimePrev;
    float3 _pad3;
    bool debug_view_albedo() {
        return frameFlags & FRAME_FLAG_DEBUG_VIEW_ALBEDO;
    }
    bool debug_view_lod() {
        return frameFlags & FRAME_FLAG_DEBUG_VIEW_LOD;
    }
    bool frustum_cull() {
        return frameFlags & FRAME_FLAG_FRUSTUM_CULL;
    }
    bool occlusion_cull() {
        return frameFlags & FRAME_FLAG_OCCLUSION_CULL;
    }
};
struct SceneMeshLod
{
    uint numIndices;
    uint numMeshlets;

    D3D12_INDEX_BUFFER_VIEW indices; // 16
    D3D12_GPU_VIRTUAL_ADDRESS meshlets; // 8
    D3D12_GPU_VIRTUAL_ADDRESS meshletTriangles;
    D3D12_GPU_VIRTUAL_ADDRESS meshletVertices;
};
struct SceneMeshInstance
{
    uint version;
    uint instanceIndex;
    uint numVertices; // 4
    uint materialIndex; //4
    uint instanceFlags; // 4
    int lodOverride; // 4

    matrix transform; // 16 * 4
    matrix transformInvTranspose; // xxx transform is sufficent for affine transformations

    matrix transformPrev; // again, previous frame

    BoundingBox boundingBox;
    BoundingSphere boundingSphere;    

    D3D12_VERTEX_BUFFER_VIEW vertices; // 16
    SceneMeshLod lods[MAX_LOD_COUNT];
    bool has_transparency() {
        return instanceFlags & INSTANCE_FLAG_TRANSPARENCY;
    }
    bool enabled() {
        return instanceFlags & INSTANCE_FLAG_VISIBLE;
    }
    bool occlusion_occluder() {
        // We don't have a Z-prepass so every geometry would be a occluder
        return true;
    }
    bool occlusion_occludee() {
        return instanceFlags & INSTANCE_FLAG_OCCLUDEE;
    }
    bool draw_bounds() {
        return instanceFlags & INSTANCE_FLAG_DRAW_BOUNDS;
    }
};
struct SceneMaterial {
    // bindless handles within ResourceDescriptorHeap
    uint albedoMap; 
    uint normalMap;
    uint pbrMap;
    uint emissiveMap; // 4

    // plain old data
    float4 albedo;
    float4 pbr;
    float4 emissive;
};
#define SCENE_LIGHT_TYPE_POINT 0
#define SCENE_LIGHT_TYPE_DIRECTIONAL 1
struct SceneLight {
    uint type;
    float intensity;
    float radius;
    float4 position;
    float4 direction;
    float4 color;    
};
struct IndirectConstant
{
    uint MeshIndex;
};
struct FFXSpdConstant { // ! align for CB    
    uint4 dstMipHeapIndex[12]; // fill mip0 with src
    uint atomicCounterHeapIndex;
    uint numMips;
    uint numWorkGroups;
    uint pad_;
    uint2 dimensions;
};
struct DepthSampleToTextureConstant {
    uint depthSRVHeapIndex;
    uint hizMip0UavHeapIndex;
    uint2 dimensions;    
};
#ifdef __cplusplus
#pragma pack(pop)
#endif
#endif