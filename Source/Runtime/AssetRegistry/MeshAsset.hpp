#pragma once
#include "IO.hpp"
#include "../RHI/RHI.hpp"

#include "Asset.hpp"
#include "MeshImporter.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"
struct VertexLayoutElement {
	const char* semantic;
	const RHI::ResourceFormat format;
};
typedef std::vector<VertexLayoutElement> VertexLayout;
inline constexpr std::vector<D3D12_INPUT_ELEMENT_DESC> VertexLayoutToD3DIADesc(const VertexLayout in) {
	std::vector<D3D12_INPUT_ELEMENT_DESC> out;
	for (auto& elem : in)
		out.push_back({elem.semantic, 0, ResourceFormatToD3DFormat(elem.format), 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
	return out;
}
struct MeshLod {
	uint numIndices;
	uint numMeshlets;

	std::vector<UINT> indexInitialData;
	std::vector<Meshlet> meshletInitialData;
	std::vector<MeshletTriangle> meshletTriangleInitialData;
	std::vector<UINT> meshletVertexInitialData;
			
	std::unique_ptr<RHI::Buffer> indexBuffer;
	std::unique_ptr<RHI::Buffer> meshletBuffer, meshletTriangleBuffer, meshletVertexBuffer;
};
template<> struct Asset<mesh_static> {
	using imported_type = mesh_static;
	static constexpr AssetType type = AssetType::StaticMesh;
	struct Vertex {
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
	uint numVertices;
	DirectX::BoundingBox boundingBox;
	DirectX::BoundingSphere boundingSphere;

	std::vector<Vertex> vertexInitialData;
	std::unique_ptr<RHI::Buffer> vertexBuffer;
	MeshLod lods[MAX_LOD_COUNT];	
	void clean() {
		vertexInitialData = {};
		for (int i = 0; i < MAX_LOD_COUNT; i++) {
			lods[i].indexInitialData = {};
			lods[i].meshletInitialData = {};
			lods[i].meshletTriangleInitialData = {};
			lods[i].meshletVertexInitialData = {};
		}
	}
	Asset(mesh_static&&);
	void upload(RHI::Device*);
};

typedef Asset<mesh_static> StaticMeshAsset;