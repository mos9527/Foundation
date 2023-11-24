#include "AssetRegistry.hpp"
#include "MeshAsset.hpp"

MeshAsset::MeshAsset(RHI::Device* device, StaticMesh* data) {
	DirectX::BoundingBox::CreateFromPoints(boundingBox, data->position.size(), data->position.data(), sizeof(float3));
	DirectX::BoundingSphere::CreateFromBoundingBox(boundingSphere, boundingBox);
	vertexBuffer = std::make_unique<MeshVertexBuffer<Vertex>>(device, data->position.size());
	for (uint i = 0; i < data->position.size(); i++) {
		auto* vertex = vertexBuffer->loadBuffer.DataAt(i);
		vertex->position = data->position[i];
		vertex->normal = data->normal[i];
		vertex->tangent = data->tangent[i];
		vertex->uv = data->uv[i];
	}
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
		lodBuffers[i] = std::make_unique<MeshLODBuffers>(device, data->lods[i].indices.size(), data->lods[i].meshlets.size(), data->lods[i].meshlet_vertices.size(), data->lods[i].meshlet_triangles.size());
		auto* lod = lodBuffers[i].get();
		lod->loadIndexBuffer.WriteData(data->lods[i].indices.data(), 0, data->lods[i].indices.size(), 0);
		lod->loadMeshletBuffer.WriteData(data->lods[i].meshlets.data(), 0, data->lods[i].meshlets.size(), 0);
		lod->loadMeshletVertexBuffer.WriteData(data->lods[i].meshlet_vertices.data(), 0, data->lods[i].meshlet_vertices.size(), 0);
		lod->loadMeshletTriangleBuffer.WriteData(data->lods[i].meshlet_triangles.data(), 0, data->lods[i].meshlet_triangles.size(), 0);
	}
}

void MeshAsset::Upload(UploadContext* ctx){
	ctx->CopyBufferRegion(&vertexBuffer->loadBuffer, &vertexBuffer->buffer, 0, 0, vertexBuffer->buffer.GetDesc().sizeInBytes());	
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
		auto* lod = lodBuffers[i].get();
		ctx->CopyBufferRegion(&lod->loadIndexBuffer, &lod->indexBuffer, 0, 0, lod->indexBuffer.GetDesc().sizeInBytes());
		ctx->CopyBufferRegion(&lod->loadMeshletBuffer, &lod->meshletBuffer, 0, 0, lod->meshletBuffer.GetDesc().sizeInBytes());
		ctx->CopyBufferRegion(&lod->loadMeshletVertexBuffer, &lod->meshletVertexBuffer, 0, 0, lod->meshletVertexBuffer.GetDesc().sizeInBytes());
		ctx->CopyBufferRegion(&lod->loadMeshletTriangleBuffer, &lod->meshletTriangleBuffer, 0, 0, lod->meshletTriangleBuffer.GetDesc().sizeInBytes());
	}
}