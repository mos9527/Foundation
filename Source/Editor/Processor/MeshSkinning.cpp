#include "MeshSkinning.hpp"
#include "../Shaders/Shared.h"
#include "../Scene/Scene.hpp"
using namespace RHI;
using namespace EditorGlobals;
MeshSkinning::MeshSkinning(Device* device) : device(device) {
	CS = BuildShader(L"MeshSkinning", L"main", L"cs_6_6");	

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *EditorGlobals::g_RHI.rootSig;
	ComPtr<ID3D12PipelineState> pso;

	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	transformedVertexBuffers.resize(MAX_INSTANCE_COUNT);

	meshSkinningTasks.first = std::make_unique<BufferContainer<MeshSkinningTask>>(device, MAX_INSTANCE_COUNT, L"Skinning Tasks");
	meshSkinningTasks.second = std::make_unique<ShaderResourceView>(meshSkinningTasks.first.get(), ShaderResourceViewDesc::GetStructuredBufferDesc(
		0, MAX_INSTANCE_COUNT, sizeof(MeshSkinningTask)
	));
};

const uint MeshSkinning::AllocateOrReuseSkinnedVertexBuffer(SceneSkinnedMeshComponent* mesh) {
	size_t index = mesh->parent.index<SceneSkinnedMeshComponent>(mesh->get_entity());
	SkinnedMeshAsset& asset = mesh->parent.get<SkinnedMeshAsset>(mesh->meshAsset);
	// Make or reuse a dedicated vertex buffer for this mesh
	if (!transformedVertexBuffers[index].first.get() || transformedVertexBuffers[index].first->GetDesc().numElements() < asset.vertexBuffer->numElements) {
		transformedVertexBuffers[index].first = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
			asset.vertexBuffer->numElements * sizeof(StaticMeshAsset::Vertex), sizeof(StaticMeshAsset::Vertex), ResourceState::UnorderedAccess,
			ResourceHeapType::Default, ResourceFlags::UnorderedAccess, L"Transformed Instance Vertices"
		));
		transformedVertexBuffers[index].second = std::make_unique<UnorderedAccessView>(
			transformedVertexBuffers[index].first.get(),
			UnorderedAccessViewDesc::GetStructuredBufferDesc(0, asset.vertexBuffer->numElements, sizeof(StaticMeshAsset::Vertex), 0)
		);
		LOG(INFO) << "Allocated new vertex buffer for skinned mesh " << mesh->name << " #" << index;
	}
	return index;
}
const std::pair<RHI::Buffer*, RHI::UnorderedAccessView*> MeshSkinning::GetSkinnedVertexBuffer(SceneSkinnedMeshComponent* mesh) {
	size_t index = AllocateOrReuseSkinnedVertexBuffer(mesh);
	auto const& buffer = transformedVertexBuffers[index];
	return { buffer.first.get(), buffer.second.get() };
}

void MeshSkinning::Begin(CommandList* ctx) {
	CHECK(taskCounter == 0 && "Dirty skinning pipeline! Did you call End(ctx)?");
	auto native = ctx->GetNativeCommandList();	
	native->SetPipelineState(*PSO);
	native->SetComputeRootSignature(*g_RHI.rootSig);
}

void MeshSkinning::Update(CommandList* ctx, SceneSkinnedMeshComponent* mesh) {
	ZoneScopedN("Skinning CPU Dispatch");
	size_t index = mesh->parent.index<SceneSkinnedMeshComponent>(mesh->get_entity());
	SkinnedMeshAsset& asset = mesh->parent.get<SkinnedMeshAsset>(mesh->meshAsset);
	auto transformedBuffer = GetSkinnedVertexBuffer(mesh);
	AssetBoneTransformComponent* boneTransform = mesh->parent.try_get<AssetBoneTransformComponent>(mesh->boneTransformComponent);
	AssetKeyshapeTransformComponent* keyshapeTransform = mesh->parent.try_get<AssetKeyshapeTransformComponent>(mesh->keyshapeTransformComponent);
	MeshSkinningTask task{
		.numBones = boneTransform ? boneTransform->data().first->NumElements() : 0,
		.boneMatricesSrv = boneTransform ? boneTransform->data().second->allocate_transient_descriptor(ctx).get_heap_handle() : INVALID_HEAP_HANDLE,

		.numShapeKeys = keyshapeTransform ? keyshapeTransform->data().first->NumElements() : 0,
		.keyshapeWeightsSrv = keyshapeTransform ? keyshapeTransform->data().second->allocate_transient_descriptor(ctx).get_heap_handle() : INVALID_HEAP_HANDLE,
		.keyshapeVerticesSrv = asset.keyShapeBuffer ? asset.keyShapeBuffer->srv->allocate_transient_descriptor(ctx).get_heap_handle() : INVALID_HEAP_HANDLE,
		.keyshapeOffsetsSrv = asset.keyShapeOffsetBuffer ? asset.keyShapeOffsetBuffer->srv->allocate_transient_descriptor(ctx).get_heap_handle() : INVALID_HEAP_HANDLE,

		.srcBufferSrv = asset.vertexBuffer->srv->allocate_transient_descriptor(ctx).get_heap_handle(),
		.srcNumVertices = asset.vertexBuffer->numElements,
		.destBufferUav = transformedBuffer.second->allocate_transient_descriptor(ctx).get_heap_handle()
	};
	*meshSkinningTasks.first->DataAt(taskCounter) = task;
	MeshSkinningConstant constants{
		.tasksSrv = meshSkinningTasks.second->allocate_transient_descriptor(ctx).get_heap_handle(),
		.taskIndex = taskCounter,		
	};
	auto native = ctx->GetNativeCommandList();	
	ctx->QueueTransitionBarrier(transformedBuffer.first, ResourceState::UnorderedAccess);
	ctx->FlushBarriers();
	native->SetComputeRoot32BitConstants(RHIContext::ROOTSIG_CB_8_CONSTANTS, 2, &constants, 0);
	native->Dispatch(DivRoundUp(asset.vertexBuffer->numElements, EDITOR_SKINNING_THREADS), 1, 1);
	ctx->QueueTransitionBarrier(transformedBuffer.first, ResourceState::VertexAndConstantBuffer);
	ctx->FlushBarriers();
	taskCounter++;
}

void MeshSkinning::End(CommandList* ctx) {
	taskCounter = 0;
}