#pragma once
#include "SceneGraph.hpp"
#include "Components/StaticMesh.hpp"
#include "Components/Camera.hpp"

#include "../RHI/RHI.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"
#include "../AssetRegistry/MeshAsset.hpp"
#include "../AssetRegistry/ResourceContainer.hpp"

class SceneGraphView {
public:
private:
	BufferContainer<SceneMeshInstance> meshInstancesBuffer;
	BufferContainer<SceneMaterial> meshMaterialsBuffer;
	BufferContainer<SceneLight> lightBuffer;	
	BufferContainer<SceneGlobals> globalBuffer;
public:
	struct FrameData {
		SceneGraph* scene;
		uint viewportWidth;
		uint viewportHeight;
		uint frameIndex;
		uint frameFlags;
		uint backBufferIndex;
		float frameTimePrev;
	};
	SceneGraphView(RHI::Device* device) : 
		meshInstancesBuffer(device, MAX_INSTANCE_COUNT),
		meshMaterialsBuffer(device, MAX_MATERIAL_COUNT),
		lightBuffer(device,MAX_LIGHT_COUNT),
		globalBuffer(device, 1)	
	{
		globalBuffer.Data()->frameFlags = FRAME_FLAG_DEFAULT;
	};

};