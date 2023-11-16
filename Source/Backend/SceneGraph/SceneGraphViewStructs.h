#ifndef SCENE_STRUCTS
#define SCENE_STRCUTS
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
struct SceneStats {
	uint numMeshes;
	uint3 _pad;
};
#define MESH_STATIC 0
#define MESH_SKINNED 1
struct SceneMesh {
	uint type;
	matrix transform;

	D3D12_VERTEX_BUFFER_VIEW vertices;
	D3D12_INDEX_BUFFER_VIEW indicesLod[MAX_LOD_COUNT];
	// xxx meshlets
};
#endif
