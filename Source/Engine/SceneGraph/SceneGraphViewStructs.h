#ifndef SCENE_STRUCTS
#define SCENE_STRUCTS
#include "../../defines.hpp"
#ifdef __cplusplus
#include "../../pch.hpp"
#else // HLSL
typedef uint2 D3D12_GPU_VIRTUAL_ADDRESS;
struct D3D12_DRAW_INDEXED_ARGUMENTS
{
	uint IndexCountPerInstance;
	uint InstanceCount;
	uint StartIndexLocation;
	int	 BaseVertexLocation;
	uint StartInstanceLocation;
};
struct D3D12_VERTEX_BUFFER_VIEW
{
	D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
	uint					  SizeInBytes;
	uint					  StrideInBytes;
};
struct D3D12_INDEX_BUFFER_VIEW
{
	D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
	uint					  SizeInBytes;
	uint					  Format;
};
#endif
struct SceneCamera {
	float4 position;

	float fov;
	float aspect;
	float nearZ;
	float farZ;
	
	float4 clipPlanes[6];

	matrix view;
	matrix projection;
	matrix viewProjection;

	matrix invView;
	matrix invProjection;
	matrix invViewProjection;
};
struct SceneGlobals {
	SceneCamera camera;

	uint numMeshes;
	uint3 _pad;
};
struct SceneMeshLod {
	D3D12_INDEX_BUFFER_VIEW indices;
	D3D12_GPU_VIRTUAL_ADDRESS meshlets;
	D3D12_GPU_VIRTUAL_ADDRESS meshletTriangles;
	D3D12_GPU_VIRTUAL_ADDRESS meshletVertices;
};
struct SceneMeshInstance {
	matrix transform;

	D3D12_VERTEX_BUFFER_VIEW vertices;
	SceneMeshLod lods[MAX_LOD_COUNT];
	// xxx meshlets
};
#endif
