#include "AssetRegistry.hpp"
#include "MeshAsset.hpp"

StaticMeshAsset::StaticMeshAsset(RHI::Device* device, StaticMesh* data) {
	DirectX::BoundingBox::CreateFromPoints(boundingBox, data->position.size(), data->position.data(), sizeof(float3));
	DirectX::BoundingSphere::CreateFromBoundingBox(boundingSphere, boundingBox);
	vertexBuffer = std::make_unique<MeshBuffer<Vertex>>(device, data->position.size());
	for (uint i = 0; i < data->num_vertices(); i++) {
		auto* vertex = vertexBuffer->loadBuffer.DataAt(i);
		vertex->position = data->position[i];
		vertex->prevPosition = vertex->position;
		vertex->normal = data->normal[i];
		vertex->tangent = data->tangent[i];
		vertex->uv = data->uv[i];
	}
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
#ifdef RHI_USE_MESH_SHADER
		lodBuffers[i] = std::make_unique<MeshLODBuffers>(device, data->lods[i].indices.size(), data->lods[i].meshlets.size(), data->lods[i].meshlet_vertices.size(), data->lods[i].meshlet_triangles.size());
#else
		lodBuffers[i] = std::make_unique<MeshLODBuffers>(device, data->lods[i].indices.size());
#endif
		auto* lod = lodBuffers[i].get();
		lod->loadIndexBuffer.WriteData(data->lods[i].indices.data(), 0, data->lods[i].indices.size(), 0);
#ifdef RHI_USE_MESH_SHADER
		lod->loadMeshletBuffer.WriteData(data->lods[i].meshlets.data(), 0, data->lods[i].meshlets.size(), 0);
		lod->loadMeshletVertexBuffer.WriteData(data->lods[i].meshlet_vertices.data(), 0, data->lods[i].meshlet_vertices.size(), 0);
		lod->loadMeshletTriangleBuffer.WriteData(data->lods[i].meshlet_triangles.data(), 0, data->lods[i].meshlet_triangles.size(), 0);
#endif
	}
	name = data->name;
}

void StaticMeshAsset::Upload(UploadContext* ctx) {
	CHECK(!isUploaded);
	ctx->CopyBufferRegion(&vertexBuffer->loadBuffer, &vertexBuffer->buffer, 0, 0, vertexBuffer->buffer.GetDesc().sizeInBytes());
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
		auto* lod = lodBuffers[i].get();
		ctx->CopyBufferRegion(&lod->loadIndexBuffer, &lod->indexBuffer, 0, 0, lod->indexBuffer.GetDesc().sizeInBytes());
#ifdef RHI_USE_MESH_SHADER
		ctx->CopyBufferRegion(&lod->loadMeshletBuffer, &lod->meshletBuffer, 0, 0, lod->meshletBuffer.GetDesc().sizeInBytes());
		ctx->CopyBufferRegion(&lod->loadMeshletVertexBuffer, &lod->meshletVertexBuffer, 0, 0, lod->meshletVertexBuffer.GetDesc().sizeInBytes());
		ctx->CopyBufferRegion(&lod->loadMeshletTriangleBuffer, &lod->meshletTriangleBuffer, 0, 0, lod->meshletTriangleBuffer.GetDesc().sizeInBytes());
#endif
	}
	isUploaded = true;
}
	
SkinnedMeshAsset::SkinnedMeshAsset(RHI::Device* device, SkinnedMesh* data) {
	vertexBuffer = std::make_unique<MeshBuffer<Vertex>>(device, data->position.size());
	for (uint i = 0; i < data->num_vertices(); i++) {
		auto* vertex = vertexBuffer->loadBuffer.DataAt(i);
		vertex->position = data->position[i];
		vertex->normal = data->normal[i];
		vertex->tangent = data->tangent[i];
		vertex->uv = data->uv[i];
		vertex->boneIndices = data->boneIndices[i];
		vertex->boneWeights = data->boneWeights[i];
	}
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
#ifdef RHI_USE_MESH_SHADER
		lodBuffers[i] = std::make_unique<MeshLODBuffers>(device, data->lods[i].indices.size(), data->lods[i].meshlets.size(), data->lods[i].meshlet_vertices.size(), data->lods[i].meshlet_triangles.size());
#else
		lodBuffers[i] = std::make_unique<MeshLODBuffers>(device, data->lods[i].indices.size());
#endif
		auto* lod = lodBuffers[i].get();
		lod->loadIndexBuffer.WriteData(data->lods[i].indices.data(), 0, data->lods[i].indices.size(), 0);
#ifdef RHI_USE_MESH_SHADER
		lod->loadMeshletBuffer.WriteData(data->lods[i].meshlets.data(), 0, data->lods[i].meshlets.size(), 0);
		lod->loadMeshletVertexBuffer.WriteData(data->lods[i].meshlet_vertices.data(), 0, data->lods[i].meshlet_vertices.size(), 0);
		lod->loadMeshletTriangleBuffer.WriteData(data->lods[i].meshlet_triangles.data(), 0, data->lods[i].meshlet_triangles.size(), 0);
#endif
	}
	if (data->keyShapes.size()) {
		uint totalKeyShapeVertices = 0;
		for (auto& keyshape : data->keyShapes) 
			totalKeyShapeVertices += keyshape.num_vertices();	
		keyShapeBuffer = std::make_unique<MeshBuffer<StaticMeshAsset::Vertex>>(device, totalKeyShapeVertices);
		keyShapeOffsetBuffer = std::make_unique<MeshBuffer<UINT>>(device, data->keyShapes.size());
		uint keyShapeVertexOffset = 0;
		for (uint k = 0; k < data->keyShapes.size(); k++) {
		
			for (uint i = 0; i < data->keyShapes[k].num_vertices(); i++) {
				auto* vertex = keyShapeBuffer->loadBuffer.DataAt(i + keyShapeVertexOffset);
				vertex->position = data->keyShapes[k].position[i];
				vertex->normal = data->keyShapes[k].normal[i];
				vertex->tangent = data->keyShapes[k].tangent[i];
				vertex->uv = data->keyShapes[k].uv[i];
			}
			*keyShapeOffsetBuffer->loadBuffer.DataAt(k) = keyShapeVertexOffset;
			keyShapeVertexOffset += data->keyShapes[k].num_vertices();
		}
	}
	name = data->name;
}

void SkinnedMeshAsset::Upload(UploadContext* ctx){
	CHECK(!isUploaded);
	ctx->CopyBufferRegion(&vertexBuffer->loadBuffer, &vertexBuffer->buffer, 0, 0, vertexBuffer->buffer.GetDesc().sizeInBytes());	
	for (uint i = 0; i < MAX_LOD_COUNT; i++) {
		auto* lod = lodBuffers[i].get();
		ctx->CopyBufferRegion(&lod->loadIndexBuffer, &lod->indexBuffer, 0, 0, lod->indexBuffer.GetDesc().sizeInBytes());
#ifdef RHI_USE_MESH_SHADER
		ctx->CopyBufferRegion(&lod->loadMeshletBuffer, &lod->meshletBuffer, 0, 0, lod->meshletBuffer.GetDesc().sizeInBytes());
		ctx->CopyBufferRegion(&lod->loadMeshletVertexBuffer, &lod->meshletVertexBuffer, 0, 0, lod->meshletVertexBuffer.GetDesc().sizeInBytes());
		ctx->CopyBufferRegion(&lod->loadMeshletTriangleBuffer, &lod->meshletTriangleBuffer, 0, 0, lod->meshletTriangleBuffer.GetDesc().sizeInBytes());
#endif
	}
	if (keyShapeBuffer) ctx->CopyBufferRegion(&keyShapeBuffer->loadBuffer, &keyShapeBuffer->buffer, 0, 0, keyShapeBuffer->buffer.GetDesc().sizeInBytes());
	if (keyShapeOffsetBuffer) ctx->CopyBufferRegion(&keyShapeOffsetBuffer->loadBuffer, &keyShapeOffsetBuffer->buffer, 0, 0, keyShapeOffsetBuffer->buffer.GetDesc().sizeInBytes());
	isUploaded = true;
}