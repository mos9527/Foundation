#pragma once
#include "Processor.hpp"
#include "../Scene/SceneComponent/SkinnedMesh.hpp"
#include "../../Runtime/Asset/ResourceContainer.hpp"
#include "../Shaders/Shared.h"
class MeshSkinning {
public:
	MeshSkinning(RHI::Device* device);

	const std::pair<RHI::Buffer*, RHI::UnorderedAccessView*> GetSkinnedVertexBuffer(SceneSkinnedMeshComponent* mesh);

	void Begin(RHI::CommandList* ctx);
	void Update(RHI::CommandList* ctx, SceneSkinnedMeshComponent* mesh);		
	void End(RHI::CommandList* ctx);
private:
	const uint AllocateOrReuseSkinnedVertexBuffer(SceneSkinnedMeshComponent* mesh);

	uint taskCounter = 0;
	std::pair<std::unique_ptr<BufferContainer<MeshSkinningTask>>,std::unique_ptr<RHI::ShaderResourceView>> meshSkinningTasks;
	
	std::unique_ptr<RHI::Shader> CS;
	std::unique_ptr<RHI::PipelineState> PSO;

	std::vector<std::pair<std::unique_ptr<RHI::Buffer>, std::unique_ptr<RHI::UnorderedAccessView>>> transformedVertexBuffers;
	RHI::Device* const device;
};
