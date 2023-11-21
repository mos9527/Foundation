#ifndef SHARED_INCLUDE
#define SHARED_INCLUDE
#define INVALID_HEAP_HANDLE ((uint)-1)
#define MAX_INSTANCE_COUNT 0xffff
#define MAX_LIGHT_COUNT 16
#define MAX_MATERIAL_COUNT 0xffff

// xxx some of these can be moved to compiler defines
// ...let's do that sometimes?
#define INVERSE_Z

#define MESHLET_MAX_VERTICES 64u // https://developer.nvidia.com/blog/introduction-turing-mecacsh-shaders/
#define MESHLET_MAX_PRIMITIVES 124u // 4b aligned

#define MAX_LOD_COUNT 8
#define LOD_GET_RATIO(lod) ((float)(MAX_LOD_COUNT - lod) / MAX_LOD_COUNT)

#define THREADS_PER_WAVE 64 // Assumes availability of wave size of 32 threads
// Pre-defined threadgroup sizes for AS & MS stages
#define MAX(x, y) (x > y ? x : y)
#define ROUNDUP(x, y) ((x + y - 1) & ~(y - 1))

#define RENDERER_INSTANCE_CULL_THREADS THREADS_PER_WAVE
#define RENDERER_FULLSCREEN_THREADS 8 // 8 * 8 -> 64

#define FRAME_FLAG_VIEW_ALBEDO (1 << 0)
#define FRAME_FLAG_GBUFFER_ALBEDO_AS_LOD (1 << 1)
#define FRAME_FLAG_DEBUG_VIEW_LOD (FRAME_FLAG_VIEW_ALBEDO | FRAME_FLAG_GBUFFER_ALBEDO_AS_LOD)
#define FRAME_FLAG_WIREFRAME (1 << 2)
#ifdef __cplusplus
#include "../../../pch.hpp"
#pragma pack(push, 4) // otherwise it's 8 on 64-bit systems
#else
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

    uint numMeshInstances;
    uint numLights;
    uint2 _pad;

    uint frameFlags;
    uint frameIndex;
    uint2 frameDimension;

    uint sceneVersion;
    uint backBufferIndex;
    uint2 _pad2;
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
    uint numVertices; // 4
    uint materialIndex; //4
    int lodOverride; // 4

    matrix transform; // 16 * 4
    matrix transformInvTranspose; // xxx transform is sufficent for affine transformations

    BoundingBox boundingBox;
    BoundingSphere boundingSphere;    

    D3D12_VERTEX_BUFFER_VIEW vertices; // 16
    SceneMeshLod lods[MAX_LOD_COUNT];
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
#ifdef __cplusplus
#pragma pack(pop)
#endif
#endif