#pragma once
#include "IO.hpp"
#include "../RHI/RHI.hpp"

#include "../../Runtime/Renderer/Shaders/Shared.h"
struct VertexLayoutElement {
	const char* semantic;
	const RHI::ResourceFormat format;
};
typedef std::vector<VertexLayoutElement> VertexLayout;

struct StaticVertex {
	float3 position;
	float3 normal;
	float3 tangent;
	float2 uv;
	static constexpr VertexLayout get_layout() {
		return { 
			{ "POSITION" ,RHI::ResourceFormat::R32G32B32_FLOAT },
			{ "NORMAL" ,RHI::ResourceFormat::R32G32B32_FLOAT },
			{ "TANGENT" ,RHI::ResourceFormat::R32G32B32_FLOAT },
			{ "TEXCOORD" ,RHI::ResourceFormat::R32G32_FLOAT }
		};
	}
};
inline constexpr std::vector<D3D12_INPUT_ELEMENT_DESC> VertexLayoutToD3DIADesc(const VertexLayout in) {
	std::vector<D3D12_INPUT_ELEMENT_DESC> out;
	for (auto& elem : in)
		out.push_back({elem.semantic, 0, ResourceFormatToD3DFormat(elem.format), 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
	return out;
}

struct Meshlet {
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
struct MeshletTriangle { UINT V0 : 10, V1 : 10, V2 : 10, : 2; };
template<typename Vertex> struct MeshAsset {
	struct mesh_lod {
		std::vector<UINT> indexInitialData;
		uint numIndices;
		std::unique_ptr<RHI::Buffer> indexBuffer;

		uint numMeshlets;
		std::vector<Meshlet> meshletInitialData;
		std::vector<MeshletTriangle> meshletTriangleInitialData;
		std::vector<UINT> meshletVertexInitialData;
		std::unique_ptr<RHI::Buffer> meshletBuffer, meshletTriangleBuffer, meshletVertexBuffer;
			
		std::unique_ptr<RHI::ShaderResourceView> indexSrv;
	};
	mesh_lod lod_buffers[MAX_LOD_COUNT];
	uint numVertices;
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
typedef MeshAsset<StaticVertex> StaticMeshAsset;