#pragma once
#include "Processor.hpp"
#include "../Scene/SceneComponent/SkinnedMesh.hpp"
#include "../../Runtime/Asset/ResourceContainer.hpp"
#include "../Shaders/Shared.h"
class SkinnedMeshBufferProcessor {
public:
	std::unique_ptr<RHI::Buffer> sceneMeshBuffer; // SceneMeshBuffer[MAX_INSTANCE_COUNT]. Index -> index in registry storage of SceneSkinnedMeshComponent

	SkinnedMeshBufferProcessor(RHI::Device* device);

	void BeginProcess();
	void RegisterOrUpdate(SceneSkinnedMeshComponent* mesh);
	void EndProcess();

private:
	uint numToUpdate = 0;
	RHI::CommandList ctx;
	std::unique_ptr<BufferContainer<SkinningConstants>> SkinConstants;
	std::unique_ptr<RHI::Shader> SkinCS, ReduceCS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> SkinPSO, ReducePSO;

	std::vector<std::unique_ptr<RHI::Buffer>> TransformedVertBuffers;
	std::unique_ptr<RHI::Buffer> reductionBuffer;
	RHI::Device* const device;
};
