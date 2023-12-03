#pragma once
#include "Processor.hpp"
#include "../Scene/SceneComponent/StaticMesh.hpp"
#include "../../Runtime/Asset/ResourceContainer.hpp"
#include "../Shaders/Shared.h"

class StaticMeshBufferProcessor {
public:
	std::unique_ptr<BufferContainer<SceneMeshBuffer>> sceneMeshBuffer; // SceneMeshBuffer[MAX_INSTANCE_COUNT]. Index -> index in registry storage of SceneStaticMeshComponent

	StaticMeshBufferProcessor(RHI::Device* device);	
	void RegisterOrUpdate(SceneStaticMeshComponent* mesh);
private:
	RHI::Device* const device;
	RenderGraphResourceCache cache;
};
#pragma once
