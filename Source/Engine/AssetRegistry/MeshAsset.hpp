#pragma once
#include "IO.hpp"
#include "../RHI/RHI.hpp"
struct static_vertex {
	float3 position;
	float3 normal;
	float3 tangent;
	float2 uv;
};
struct meshlet {
	uint vertex_offset;
	uint vertex_count;

	uint triangle_offset;
	uint triangle_count;

	/* bounding sphere, useful for frustum and occlusion culling */
	float center[3];
	float radius;

	/* normal cone, useful for backface culling */
	float cone_apex[3];
	uint cone_axis_cutoff; // cutoff | z | y | x
};
struct meshlet_triangle { UINT V0 : 10, V1 : 10, V2 : 10, : 2; };
template<typename Vertex> struct MeshAsset {
	struct mesh_lod {
		std::vector<UINT> indexInitialData;
			
		std::unique_ptr<RHI::Buffer> indexBuffer;

		std::vector<meshlet> meshletInitialData;
		std::vector<meshlet_triangle> meshletTriangleInitialData;
		std::vector<UINT> meshletVertexInitialData;
		std::unique_ptr<RHI::Buffer> meshletBuffer, meshletTriangleBuffer, meshletVertexBuffer;
			
		std::unique_ptr<RHI::ShaderResourceView> indexSrv;
	};
	mesh_lod lod_buffers[MAX_LOD_COUNT];

	std::vector<Vertex> vertexInitialData; // xxx
	std::unique_ptr<RHI::Buffer> vertexBuffer;
	std::unique_ptr<RHI::ShaderResourceView> vertexSrv;
		
	void clean() {
		vertexInitialData = {};
		for (int i = 0; i < MAX_LOD_COUNT; i++) {
			lod_buffers[i].indexInitialData = {};
			lod_buffers[i].meshletInitialData = {};
			lod_buffers[i].meshletTriangleInitialData = {};
			lod_buffers[i].meshletVertexInitialData = {};
		}
	}
};
typedef MeshAsset<static_vertex> StaticMeshAsset;