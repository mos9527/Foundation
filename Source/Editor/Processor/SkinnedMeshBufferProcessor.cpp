#include "SkinnedMeshBufferProcessor.hpp"
#include "../Shaders/Shared.h"
#include "../Scene/Scene.hpp"

using namespace RHI;
SkinnedMeshBufferProcessor::SkinnedMeshBufferProcessor(Device* device) : device(device) {
	SkinCS = BuildShader(L"MeshSkinning", L"main_skinning", L"cs_6_6");
	ReduceCS = BuildShader(L"MeshSkinning", L"main_reduce", L"cs_6_6");
	RS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.AddUnorderedAccessView(0, 0) // 0 - u0 space0 : Out Vertices
		.AddUnorderedAccessView(1, 0) // 1 - u1 space0 : Out MeshBuffer
		.AddShaderResourceView(0, 0)  // 2 - t0 space0 : Skinning Constants
		.AddShaderResourceView(1, 0) //  3 - t1 space0 : In Vertices
		.AddShaderResourceView(2, 0) //  4 - t2 space0 : In Bone Matrices     | (Upload Heap)
		.AddShaderResourceView(3, 0) //  5 - t3 space0 : In KeyShape Weights  | (Upload Heap)
		.AddShaderResourceView(4, 0) //  6 - t4 space0 : In KeyShape Vertices
		.AddShaderResourceView(5, 0) //  7 - t5 space0 : In KeyShape Offsets	
		.AddConstant(0,0,2)          //  8 - b0 space0 : Mesh Index, Pass Index
		.AddUnorderedAccessView(2,0) //  9 - u2 space0 : BBox reduction buffer
	);
	RS->SetName(L"Skinning");
	
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *RS;
	ComPtr<ID3D12PipelineState> pso;

	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(SkinCS->GetData(), SkinCS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	SkinPSO = std::make_unique<PipelineState>(device, std::move(pso));

	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(ReduceCS->GetData(), ReduceCS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	ReducePSO = std::make_unique<PipelineState>(device, std::move(pso));

	sceneMeshBuffer = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(SceneMeshBuffer) * MAX_INSTANCE_COUNT, sizeof(SceneMeshBuffer), 
		ResourceState::UnorderedAccess, ResourceHeapType::Default, ResourceFlags::UnorderedAccess, L"Skinned Mesh VB/IB/BBox Buffer"
	));	
	SkinConstants = std::make_unique<BufferContainer<SkinningConstants>>(device, MAX_INSTANCE_COUNT, L"Skinning Constants");
	reductionBuffer = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		/* minX,Y,Z maxX,Y,Z */ 2 * 3 * MAX_INSTANCE_COUNT, 0,
		ResourceState::UnorderedAccess, ResourceHeapType::Default, ResourceFlags::UnorderedAccess, L"BBox Minmax Buffer"
	));
	TransformedVertBuffers.resize(MAX_INSTANCE_COUNT);
};
void SkinnedMeshBufferProcessor::RegisterOrUpdate(RHI::CommandList* ctx, SceneSkinnedMeshComponent* mesh) {	
	ZoneScopedN("Skinning CPU Dispatch");
	size_t index = mesh->parent.index<SceneSkinnedMeshComponent>(mesh->get_entity());	
	SkinnedMeshAsset& asset = mesh->parent.get<SkinnedMeshAsset>(mesh->meshAsset);	
	AssetBoneTransformComponent* boneTransform = mesh->parent.try_get<AssetBoneTransformComponent>(mesh->boneTransformComponent);
	AssetKeyshapeTransformComponent* keyshapeTransform = mesh->parent.try_get<AssetKeyshapeTransformComponent>(mesh->keyshapeTransformComponent);
	// Make a dedicated vertex buffer for this mesh
	const size_t minVertexBufferSize = asset.vertexBuffer->numElements * sizeof(StaticMeshAsset::Vertex);
	if (!TransformedVertBuffers[index].get() || TransformedVertBuffers[index]->GetDesc().sizeInBytes() < minVertexBufferSize) {
		TransformedVertBuffers[index] = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
			minVertexBufferSize, sizeof(StaticMeshAsset::Vertex), ResourceState::UnorderedAccess,
			ResourceHeapType::Default, ResourceFlags::UnorderedAccess, L"Transformed Instance Vertices"
		));
		LOG(INFO) << "Allocated new vertex buffer for " << mesh->name << " #" << index;
	}
	auto native = ctx->GetNativeCommandList();
	SkinConstants->DataAt(index)->meshBufferIndex = index;
	SkinConstants->DataAt(index)->numVertices = asset.vertexBuffer->numElements;
	SkinConstants->DataAt(index)->numBones = boneTransform ? boneTransform->get_bone_count() : 0;
	SkinConstants->DataAt(index)->numShapeKeys = keyshapeTransform ? keyshapeTransform->get_keyshape_count() : 0;
	// Fill in buffer index data to let CS copy onto the Default heap
	SkinConstants->DataAt(index)->VB = D3D12_VERTEX_BUFFER_VIEW{
		.BufferLocation = TransformedVertBuffers[index]->GetGPUAddress(),
		.SizeInBytes = (UINT)TransformedVertBuffers[index]->GetDesc().sizeInBytes(),
		.StrideInBytes = (UINT)TransformedVertBuffers[index]->GetDesc().stride
	};
	for (int i = 0; i < MAX_LOD_COUNT; i++) {
		SkinConstants->DataAt(index)->IB[i].indices = D3D12_INDEX_BUFFER_VIEW{
			.BufferLocation = asset.lodBuffers[i]->indexBuffer.GetGPUAddress(),
			.SizeInBytes = (UINT)asset.lodBuffers[i]->indexBuffer.GetDesc().sizeInBytes(),
			.Format = DXGI_FORMAT_R32_UINT
		};
		SkinConstants->DataAt(index)->IB[i].numIndices = asset.lodBuffers[i]->numIndices;
#ifdef RHI_USE_MESH_SHADER
		SkinConstants->DataAt(index)->IB[i].meshlets = asset.lodBuffers[i]->meshletBuffer.GetGPUAddress();
		SkinConstants->DataAt(index)->IB[i].meshletVertices = asset.lodBuffers[i]->meshletVertexBuffer.GetGPUAddress();
		SkinConstants->DataAt(index)->IB[i].meshletTriangles = asset.lodBuffers[i]->meshletTriangleBuffer.GetGPUAddress();
		SkinConstants->DataAt(index)->IB[i].numMeshlets = asset.lodBuffers[i]->numMeshlets;
#endif
	}
	// Skin
	native->SetPipelineState(*SkinPSO);
	native->SetComputeRootSignature(*RS);
	native->SetComputeRootUnorderedAccessView(0, TransformedVertBuffers[index]->GetGPUAddress());
	// native->SetComputeRootUnorderedAccessView(1, sceneMeshBuffer->GetGPUAddress());
	native->SetComputeRootShaderResourceView(2, SkinConstants->GetGPUAddress());
	native->SetComputeRootShaderResourceView(3, asset.vertexBuffer->buffer.GetGPUAddress());
	if (SkinConstants->DataAt(index)->numBones) native->SetComputeRootShaderResourceView(4, boneTransform->data()->GetGPUAddress());
	if (SkinConstants->DataAt(index)->numShapeKeys) native->SetComputeRootShaderResourceView(5, keyshapeTransform->data()->GetGPUAddress());
	if (asset.keyShapeBuffer) native->SetComputeRootShaderResourceView(6, asset.keyShapeBuffer->buffer.GetGPUAddress());
	if (asset.keyShapeOffsetBuffer) native->SetComputeRootShaderResourceView(7, asset.keyShapeOffsetBuffer->buffer.GetGPUAddress());
	native->SetComputeRoot32BitConstant(8, index, 0);
	native->SetComputeRootUnorderedAccessView(9, reductionBuffer->GetGPUAddress());
	ctx->QueueTransitionBarrier(sceneMeshBuffer.get(), ResourceState::UnorderedAccess);
	ctx->QueueTransitionBarrier(TransformedVertBuffers[index].get(), ResourceState::UnorderedAccess);
	ctx->FlushBarriers();
	native->Dispatch(DivRoundUp(asset.vertexBuffer->numElements, EDITOR_SKINNING_THREADS), 1, 1);
	ctx->QueueUAVBarrier(TransformedVertBuffers[index].get());
	ctx->FlushBarriers();
	// Reduce
	native->SetPipelineState(*ReducePSO);
	native->SetComputeRootSignature(*RS);
	native->SetComputeRootUnorderedAccessView(0, TransformedVertBuffers[index]->GetGPUAddress());
	native->SetComputeRootUnorderedAccessView(1, sceneMeshBuffer->GetGPUAddress());
	native->SetComputeRootShaderResourceView(2, SkinConstants->GetGPUAddress());
	native->SetComputeRoot32BitConstant(8, 0, 1); // early
	native->Dispatch(1, 1, 1);
	ctx->QueueUAVBarrier(reductionBuffer.get());
	ctx->FlushBarriers();
	native->SetComputeRoot32BitConstant(8, 1, 1); // mid
	native->Dispatch(DivRoundUp(asset.vertexBuffer->numElements, EDITOR_SKINNING_THREADS), 1, 1);
	ctx->QueueUAVBarrier(reductionBuffer.get());
	ctx->FlushBarriers();
	native->SetComputeRoot32BitConstant(8, 2, 1); // late
	native->Dispatch(1, 1, 1);
	ctx->QueueUAVBarrier(sceneMeshBuffer.get());
	ctx->QueueTransitionBarrier(TransformedVertBuffers[index].get(), ResourceState::VertexAndConstantBuffer);
	ctx->FlushBarriers();	
}