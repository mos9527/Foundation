#pragma once
#include "Asset.hpp"
#include "MeshImporter.hpp"
#include "ResourceContainer.hpp"
#include "UploadContext.hpp"
#include "../RHI/RHI.hpp"
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

template<typename Elem> static const RHI::Resource::ResourceDesc GetDefaultMeshBufferDesc(uint numElements) {
	return RHI::Resource::ResourceDesc::GetGenericBufferDesc(
		numElements * sizeof(Elem), sizeof(Elem),
		RHI::ResourceState::CopyDest, RHI::ResourceHeapType::Default,
		RHI::ResourceFlags::None
	);
}

struct MeshAsset;
struct MeshLODBuffers {
	friend struct MeshAsset;
private:
	// Upload heap
	BufferContainer<UINT> loadIndexBuffer;
#ifdef RHI_USE_MESH_SHADER
	BufferContainer<UINT> loadMeshletVertexBuffer;
	BufferContainer<Meshlet> loadMeshletBuffer;
	BufferContainer<MeshletTriangle> loadMeshletTriangleBuffer;
#endif
public:
	const uint numIndices, numMeshlets, numMeshletVertices, numMeshletTriangles;
	// Default heap
	RHI::Buffer indexBuffer;
#ifdef RHI_USE_MESH_SHADER
	RHI::Buffer meshletVertexBuffer, meshletBuffer, meshletTriangleBuffer;
	MeshLODBuffers(RHI::Device* device, uint numIndices, uint numMeshlets, uint numMeshletVertices, uint numMeshletTriangles) :
#else
	MeshLODBuffers(RHI::Device* device, uint numIndices) :
#endif
		numIndices(numIndices),
		numMeshlets(numMeshlets),
		numMeshletVertices(numMeshletVertices),
		numMeshletTriangles(numMeshletTriangles),
		loadIndexBuffer(device, numIndices),
		indexBuffer(device, GetDefaultMeshBufferDesc<UINT>(numIndices)) 
#ifdef RHI_USE_MESH_SHADER
		,loadMeshletBuffer(device, numMeshlets),
		loadMeshletVertexBuffer(device, numMeshletVertices),
		loadMeshletTriangleBuffer(device, numMeshletTriangles),
		meshletBuffer(device, GetDefaultMeshBufferDesc<UINT>(numMeshlets)),
		meshletVertexBuffer(device, GetDefaultMeshBufferDesc<UINT>(numMeshletVertices)),
		meshletTriangleBuffer(device, GetDefaultMeshBufferDesc<UINT>(numMeshletTriangles))
#endif		
		{};
	void Clean() {
		loadIndexBuffer.Release();
#ifdef RHI_USE_MESH_SHADER
		loadMeshletVertexBuffer.Release();
		loadMeshletBuffer.Release();
		loadMeshletTriangleBuffer.Release();
#endif
	}
};

template<typename VertexType> struct MeshVertexBuffer {
	friend struct MeshAsset;
private:
	BufferContainer<VertexType> loadBuffer;
public:
	RHI::Buffer buffer;
	const uint numVertices;
	MeshVertexBuffer(RHI::Device* device, uint numVertices) :
		loadBuffer(device, numVertices), buffer(device, GetDefaultMeshBufferDesc<VertexType>(numVertices)), numVertices(numVertices) {};
	void Clean() { loadBuffer.Release(); }
};

struct MeshAsset {
public:
	const static AssetType type = AssetType::StaticMesh;
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
private:
	bool isUploaded = false;
public:
	DirectX::BoundingBox boundingBox;
	DirectX::BoundingSphere boundingSphere;	
	std::unique_ptr<MeshLODBuffers> lodBuffers[MAX_LOD_COUNT];	
	std::unique_ptr<MeshVertexBuffer<Vertex>> vertexBuffer;
	
	MeshAsset(RHI::Device* device, StaticMesh* data);	
	bool IsUploaded() { return isUploaded; }
	void Upload(UploadContext* ctx);
	void Clean() {
		vertexBuffer->Clean();
		for (uint i = 0; i < MAX_LOD_COUNT; i++)
			lodBuffers[i]->Clean();
	}
};

template<> struct ImportedAssetTraits<StaticMesh> {
	using imported_by = MeshAsset;
	static constexpr AssetType type = AssetType::StaticMesh;
};