#include "SceneView.hpp"

#include "../Processor/HDRIProbe.hpp"
#include "../Processor/MeshSkinning.hpp"
#include "../Processor/LTCFittedLUT.hpp"

SceneView::SceneView(RHI::Device* device) :
	meshInstancesBuffer(device, MAX_INSTANCE_COUNT),
	meshMaterialsBuffer(device, MAX_MATERIAL_COUNT),
	lightBuffer(device, MAX_LIGHT_COUNT),
	globalBuffer(device, 1)
{
	meshSkinning = new MeshSkinning(device);
};
SceneView::~SceneView() {
	delete meshSkinning;
}

bool SceneView::update(RHI::CommandList* ctx,Scene& scene, SceneCameraComponent& camera, FrameData&& _frameData, ShaderData&& _shaderData) {
	return false;
}