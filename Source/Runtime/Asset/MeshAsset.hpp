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
struct StaticMeshAsset;
struct SkinnedMeshAsset;
struct MeshLODBuffers {
	friend StaticMeshAsset;
	friend SkinnedMeshAsset;
private:
	// Upload heap
	BufferContainer<UINT> loadIndexBuffer;
#ifdef RHI_USE_MESH_SHADER
	BufferContainer<UINT> loadMeshletVertexBuffer;
	BufferContainer<Meshlet> loadMeshletBuffer;
	BufferContainer<MeshletTriangle> loadMeshletTriangleBuffer;
#endif
public:
#ifdef RHI_USE_MESH_SHADER
	const uint numMeshlets, numMeshletVertices, numMeshletTriangles;
#endif
	const uint numIndices;
	// Default heap
	RHI::Buffer indexBuffer;
#ifdef RHI_USE_MESH_SHADER
	RHI::Buffer meshletVertexBuffer, meshletBuffer, meshletTriangleBuffer;
	MeshLODBuffers(RHI::Device* device, uint numIndices, uint numMeshlets, uint numMeshletVertices, uint numMeshletTriangles) :
#else
	MeshLODBuffers(RHI::Device* device, uint numIndices) :
#endif
		numIndices(numIndices),
		loadIndexBuffer(device, numIndices),
		indexBuffer(device, GetDefaultMeshBufferDesc<UINT>(numIndices)) 
#ifdef RHI_USE_MESH_SHADER
		,numMeshlets(numMeshlets),
		numMeshletVertices(numMeshletVertices),
		numMeshletTriangles(numMeshletTriangles),
		loadMeshletBuffer(device, numMeshlets),
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

template<typename Elem> struct MeshBuffer {
	friend StaticMeshAsset;
	friend SkinnedMeshAsset;
private:
	BufferContainer<Elem> loadBuffer;
public:
	RHI::Buffer buffer;
	std::unique_ptr<RHI::ShaderResourceView> srv;

	const uint numElements;
	MeshBuffer(RHI::Device* device, uint numVertices) : loadBuffer(device, numVertices), buffer(device, GetDefaultMeshBufferDesc<Elem>(numVertices)), numElements(numVertices) 
	{
		srv = std::make_unique<RHI::ShaderResourceView>(&buffer, RHI::ShaderResourceViewDesc::GetStructuredBufferDesc(0, numVertices, sizeof(Elem)));
	};
	void Clean() { loadBuffer.Release(); }
};

struct StaticMeshAsset {
public:
	const static AssetType type = AssetType::StaticMesh;
	struct Vertex {
		float3 position;
		float3 prevPosition; // Used to generate motion buffer
		float3 normal;
		float3 tangent;
		float2 uv;
		static constexpr VertexLayout get_layout() {
			return {
				{ "POSITION" ,RHI::ResourceFormat::R32G32B32_FLOAT },
				{ "PREVPOSITION" ,RHI::ResourceFormat::R32G32B32_FLOAT },
				{ "NORMAL" ,RHI::ResourceFormat::R32G32B32_FLOAT },
				{ "TANGENT" ,RHI::ResourceFormat::R32G32B32_FLOAT },
				{ "TEXCOORD" ,RHI::ResourceFormat::R32G32_FLOAT }
			};
		}
	};
private:
	std::string name;
	bool isUploaded = false;
public:
	DirectX::BoundingBox boundingBox;
	DirectX::BoundingSphere boundingSphere;	
	std::unique_ptr<MeshLODBuffers> lodBuffers[MAX_LOD_COUNT];	
	std::unique_ptr<MeshBuffer<Vertex>> vertexBuffer;
	
	StaticMeshAsset(RHI::Device* device, StaticMesh* data);	
	inline void SetName(const char* name_) { name = name_; }
	inline const char* GetName() { return name.c_str(); }
	inline bool IsUploaded() { return isUploaded; }
	void Upload(UploadContext* ctx);
	inline void Clean() {
		vertexBuffer->Clean();
		for (uint i = 0; i < MAX_LOD_COUNT; i++)
			lodBuffers[i]->Clean();
	}
};

template<> struct ImportedAssetTraits<StaticMesh> {
	using imported_by = StaticMeshAsset;
	static constexpr AssetType type = AssetType::StaticMesh;
};

struct SkinnedMeshAsset {
public:
	const static AssetType type = AssetType::SkinnedMesh;
	struct Vertex {
		float3 position;
		float3 normal;
		float3 tangent;
		float2 uv;
		uint4 boneIndices;
		float4 boneWeights;
		static constexpr VertexLayout get_layout() { /* UNUSED */
			return {
				{ "POSITION" ,RHI::ResourceFormat::R32G32B32_FLOAT },
				{ "NORMAL" ,RHI::ResourceFormat::R32G32B32_FLOAT },
				{ "TANGENT" ,RHI::ResourceFormat::R32G32B32_FLOAT },
				{ "TEXCOORD" ,RHI::ResourceFormat::R32G32_FLOAT },
				{ "BONEINDICES",RHI::ResourceFormat::R32G32B32A32_UINT },
				{ "BONEWEIGHTS",RHI::ResourceFormat::R32G32B32A32_FLOAT }
			};
		}
	};
private:
	std::string name;
	bool isUploaded = false;
public:
	// No longer applicable. These are to be computed per update
	// DirectX::BoundingBox boundingBox;
	// DirectX::BoundingSphere boundingSphere;
	std::unique_ptr<MeshBuffer<Vertex>> vertexBuffer;
	std::unique_ptr<MeshBuffer<StaticMeshAsset::Vertex>> keyShapeBuffer; // All keyshapes' vertices stored linearly in order given by SkinnedMesh's import
	std::unique_ptr<MeshBuffer<UINT>> keyShapeOffsetBuffer; // indexed by this buffer.
	std::unique_ptr<MeshLODBuffers> lodBuffers[MAX_LOD_COUNT];	

	SkinnedMeshAsset(RHI::Device* device, SkinnedMesh* data);
	inline void SetName(const char* name_) { name = name_; }
	inline const char* GetName() { return name.c_str(); }
	inline bool IsUploaded() { return isUploaded; }
	void Upload(UploadContext* ctx);
	inline void Clean() {
		vertexBuffer->Clean();
		keyShapeBuffer->Clean();
		keyShapeOffsetBuffer->Clean();
		for (uint i = 0; i < MAX_LOD_COUNT; i++)
			lodBuffers[i]->Clean();
	}
};

template<> struct ImportedAssetTraits<SkinnedMesh> {
	using imported_by = SkinnedMeshAsset;
	static constexpr AssetType type = AssetType::SkinnedMesh;
};
