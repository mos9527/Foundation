#include "AssetRegistry.hpp"
#include "MeshAsset.hpp"

Asset<mesh_static>::Asset(mesh_static&& mesh) {
	std::vector<StaticMeshAsset::Vertex> vertices(mesh.position.size());
	for (uint i = 0; i < mesh.position.size(); i++) {
		auto& vertex = vertices[i];
		vertex.position = mesh.position[i];
		vertex.normal = mesh.normal[i];
		vertex.tangent = mesh.tangent[i];
		vertex.uv = mesh.uv[i];
		vertexInitialData.push_back(vertex);
	}
	numVertices = vertexInitialData.size();
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
		auto& buffer = lods[i];
		buffer.indexInitialData = mesh.lods[i].indices;
		buffer.meshletInitialData = mesh.lods[i].meshlets;
		buffer.meshletVertexInitialData = mesh.lods[i].meshlet_vertices;
		buffer.meshletTriangleInitialData = mesh.lods[i].meshlet_triangles;
		buffer.numIndices = buffer.indexInitialData.size();
		buffer.numMeshlets = buffer.meshletInitialData.size();
	}
}

void StaticMeshAsset::upload(RHI::Device* device) {
	auto create_buffer_desc = [&](auto buffer) {
		return RHI::Resource::ResourceDesc::GetGenericBufferDesc(
			::size_in_bytes(buffer), sizeof(decltype(buffer)::value_type),
			RHI::ResourceState::Common, RHI::ResourceHeapType::Default,
			RHI::ResourceFlags::None
		);
	};
	auto upload_buffer = [&](auto buffer, auto dst) {
		device->Upload(dst, (void*)buffer.data(), ::size_in_bytes(buffer));
	};
	vertexBuffer = std::make_unique<RHI::Buffer>(device, create_buffer_desc(vertexInitialData));	
	upload_buffer(vertexInitialData, vertexBuffer.get());
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
		auto& buffer = lods[i];
		buffer.indexBuffer = std::make_unique<RHI::Buffer>(device, create_buffer_desc(buffer.indexInitialData));
		buffer.meshletBuffer = std::make_unique<RHI::Buffer>(device, create_buffer_desc(buffer.meshletInitialData));
		buffer.meshletTriangleBuffer = std::make_unique<RHI::Buffer>(device, create_buffer_desc(buffer.meshletTriangleInitialData));
		buffer.meshletVertexBuffer = std::make_unique<RHI::Buffer>(device, create_buffer_desc(buffer.meshletVertexInitialData));

		upload_buffer(buffer.indexInitialData, buffer.indexBuffer.get());
		upload_buffer(buffer.meshletInitialData, buffer.meshletBuffer.get());
		upload_buffer(buffer.meshletTriangleInitialData, buffer.meshletTriangleBuffer.get());
		upload_buffer(buffer.meshletVertexInitialData, buffer.meshletVertexBuffer.get());
	}
}