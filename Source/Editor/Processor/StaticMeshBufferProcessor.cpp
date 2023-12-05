#include "StaticMeshBufferProcessor.hpp"
#include "../Shaders/Shared.h"
#include "../Scene/Scene.hpp"
using namespace RHI;
StaticMeshBufferProcessor::StaticMeshBufferProcessor(Device* device) : device(device) {
	sceneMeshBuffer = std::make_unique<BufferContainer<SceneMeshBuffer>>(device, MAX_INSTANCE_COUNT);
};
void StaticMeshBufferProcessor::RegisterOrUpdate(SceneStaticMeshComponent* mesh) {
	size_t index = mesh->parent.index<SceneStaticMeshComponent>(mesh->get_entity());	
	StaticMeshAsset& asset = mesh->parent.get<StaticMeshAsset>(mesh->meshAsset);
	// Everything about the vertices is Static.
	// We can simply copy these through the CPU to the upload heap
	// and use as-is.
	sceneMeshBuffer->DataAt(index)->boundingBox = asset.boundingBox;
	sceneMeshBuffer->DataAt(index)->VB = D3D12_VERTEX_BUFFER_VIEW{
		.BufferLocation = asset.vertexBuffer->buffer.GetGPUAddress(),
		.SizeInBytes = (UINT)asset.vertexBuffer->buffer.GetDesc().sizeInBytes(),
		.StrideInBytes = (UINT)asset.vertexBuffer->buffer.GetDesc().stride
	};
	for (int i = 0; i < MAX_LOD_COUNT; i++) {
		sceneMeshBuffer->DataAt(index)->IB[i].indices = D3D12_INDEX_BUFFER_VIEW{
			.BufferLocation = asset.lodBuffers[i]->indexBuffer.GetGPUAddress(),
			.SizeInBytes = (UINT)asset.lodBuffers[i]->indexBuffer.GetDesc().sizeInBytes(),
			.Format = DXGI_FORMAT_R32_UINT
		};
		sceneMeshBuffer->DataAt(index)->IB[i].numIndices = asset.lodBuffers[i]->numIndices;
#ifdef RHI_USE_MESH_SHADER
		sceneMeshBuffer->DataAt(index)->IB[i].meshlets = asset.lodBuffers[i]->meshletBuffer.GetGPUAddress();
		sceneMeshBuffer->DataAt(index)->IB[i].meshletVertices = asset.lodBuffers[i]->meshletVertexBuffer.GetGPUAddress();
		sceneMeshBuffer->DataAt(index)->IB[i].meshletTriangles = asset.lodBuffers[i]->meshletTriangleBuffer.GetGPUAddress();
		sceneMeshBuffer->DataAt(index)->IB[i].numMeshlets = asset.lodBuffers[i]->numMeshlets;
#endif
	}
}
