#include "AssetManager.hpp"
#include "MeshImporter.hpp"
#include "MeshAsset.hpp"


namespace IO {
	template<> asset_handle AssetManager::load<StaticMeshAsset>(mesh_static& mesh) {
		auto entity = registry.create();
		asset_handle resource(entity, asset_handle::StaticMesh);
		registry.emplace<StaticMeshAsset>(entity);
		auto& asset = registry.get<StaticMeshAsset>(entity);
		
		std::vector<static_vertex> vertices(mesh.position.size());
		for (uint i = 0; i < mesh.position.size(); i++) {
			static_vertex& vertex = vertices[i];
			vertex.position = mesh.position[i];
			vertex.normal = mesh.normal[i];
			vertex.tangent = mesh.tangent[i];
			vertex.uv = mesh.uv[i];
			asset.vertexInitialData.push_back(vertex);
		}
		for (uint i = 0; i < MAX_LOD_COUNT; i++) {
			auto& buffer = asset.lod_buffers[i]; 
			buffer.indexInitialData = mesh.lods[i].indices;
			buffer.meshletInitialData = mesh.lods[i].meshlets;
			buffer.meshletVertexInitialData = mesh.lods[i].meshlet_vertices;
			buffer.meshletTriangleInitialData = mesh.lods[i].meshlet_triangles;
		}		
		return resource;
	}

	template<> void AssetManager::upload<StaticMeshAsset>(RHI::Device* device, StaticMeshAsset* asset) {		
		auto create_buffer_desc = [&](auto buffer) {
			return RHI::Resource::ResourceDesc::GetGenericBufferDesc(
				::size_in_bytes(buffer), sizeof(decltype(buffer)::value_type),
				RHI::ResourceState::Common, RHI::ResourceUsage::Default,
				RHI::ResourceFlags::None
			);
		};
		auto upload_buffer = [&](auto buffer, auto dst) {
			device->Upload(dst, (void*)buffer.data(), ::size_in_bytes(buffer));
		};
		asset->vertexBuffer = std::make_unique<RHI::Resource>(device, create_buffer_desc(asset->vertexInitialData));
		upload_buffer(asset->vertexInitialData, asset->vertexBuffer.get());
		// asset->vertexSrv = std::make_unique<RHI::ShaderResourceView>(asset->vertexBuffer.get(), RHI::ShaderResourceViewDesc{});
		// xxx
		for (uint i = 0; i < MAX_LOD_COUNT; i++) {
			auto& buffer = asset->lod_buffers[i];
			buffer.indexBuffer = std::make_unique<RHI::Resource>(device, create_buffer_desc(buffer.indexInitialData));
			buffer.meshletBuffer = std::make_unique<RHI::Resource>(device, create_buffer_desc(buffer.meshletInitialData));
			buffer.meshletTriangleBuffer = std::make_unique<RHI::Resource>(device, create_buffer_desc(buffer.meshletTriangleInitialData));
			buffer.meshletVertexBuffer = std::make_unique<RHI::Resource>(device, create_buffer_desc(buffer.meshletVertexInitialData));

			upload_buffer(buffer.indexInitialData, buffer.indexBuffer.get());
			upload_buffer(buffer.meshletInitialData, buffer.meshletBuffer.get());
			upload_buffer(buffer.meshletTriangleInitialData, buffer.meshletTriangleBuffer.get());
			upload_buffer(buffer.meshletVertexInitialData, buffer.meshletVertexBuffer.get());

			// buffer.indexSrv = std::make_unique<RHI::ShaderResourceView>(asset->vertexBuffer.get(), RHI::ShaderResourceViewDesc{}); 
			// xxx meshlet SRVs
		}
	};
}