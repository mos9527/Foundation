#ifndef COMMON_INCLUDE
#define COMMON_INCLUDE
#define MAX_INSTANCE_COUNT 0xffff
#define DISPATCH_GROUP_COUNT 0xffff

#define MESHLET_MAX_VERTICES 64u // https://developer.nvidia.com/blog/introduction-turing-mecacsh-shaders/
#define MESHLET_MAX_PRIMITIVES 124u // 4b aligned

#define MAX_LOD_COUNT 8
#define LOD_GET_RATIO(lod) ((float)(MAX_LOD_COUNT - lod) / MAX_LOD_COUNT)

#define THREADS_PER_WAVE 32 // Assumes availability of wave size of 32 threads
// Pre-defined threadgroup sizes for AS & MS stages
#define MAX(x, y) (x > y ? x : y)
#define ROUNDUP(x, y) ((x + y - 1) & ~(y - 1))

#define RENDERER_INSTANCE_CULL_THREADS THREADS_PER_WAVE
#ifdef __cplusplus
#include "../../../pch.hpp"
#else // HLSL

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
struct SceneCamera
{
    float4 position; // 16

    float fov;
    float aspect;
    float nearZ;
    float farZ; // 16
	
    float4 clipPlanes[6]; // 16 * 6

    matrix view; 
    matrix projection;
    matrix viewProjection;

    matrix invView;
    matrix invProjection;
    matrix invViewProjection;
};
struct SceneGlobals
{
    SceneCamera camera;

    uint numMeshInstances;
    uint frameIndex;
    uint2 pad_;
};
struct SceneMeshLod
{
    uint numIndices;
    uint numMeshlets;
    uint2 _pad1;

    D3D12_INDEX_BUFFER_VIEW indices; // 16
    D3D12_GPU_VIRTUAL_ADDRESS meshlets; // 8
    uint2 _pad2;
    D3D12_GPU_VIRTUAL_ADDRESS meshletTriangles;
    uint2 _pad3;
    D3D12_GPU_VIRTUAL_ADDRESS meshletVertices;
    uint2 _pad4;
};
struct SceneMeshInstance
{
    uint numVertices; // 4
    uint3 _pad1;
    matrix transform; // 16 * 4

    D3D12_VERTEX_BUFFER_VIEW vertices; // 16
    SceneMeshLod lods[MAX_LOD_COUNT];
	// xxx meshlets
};
#endif