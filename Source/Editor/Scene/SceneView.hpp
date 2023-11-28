#pragma once
#include "../../Runtime/RHI/RHI.hpp"
#include "../../Runtime/Asset/ResourceContainer.hpp"
#include "../Shaders/Shared.h"

#include "Scene.hpp"
#include "SceneComponent/Mesh.hpp"
#include "SceneComponent/Camera.hpp"
#include "SceneComponent/Light.hpp"
#include "AssetComponet/Material.hpp"
#include "AssetComponet/Mesh.hpp"

class IBLProbeProcessor;
class SceneView {
public:
private:
	BufferContainer<SceneMeshInstance> meshInstancesBuffer;
	BufferContainer<SceneMaterial> meshMaterialsBuffer;
	BufferContainer<SceneLight> lightBuffer;
	BufferContainer<SceneGlobals> globalBuffer;

	std::unordered_map<entt::entity, size_t> scenecomponent_versions;
public:
	struct ShaderData {
		struct IBLProbeData {
			bool use = true;
			IBLProbeProcessor* probe = nullptr;

			float diffuseIntensity = 1.0f;
			float specularIntensity = 1.0f;
			float occlusionStrength = 1.0f;
		} probe;
	};
	struct FrameData {
		uint viewportWidth;
		uint viewportHeight;
		uint frameIndex;
		uint frameFlags;
		uint backBufferIndex;
		float frameTimePrev;
	};
	SceneView(const SceneView&) = delete;
	SceneView(SceneView&&) = default;
	SceneView(RHI::Device* device) :
		meshInstancesBuffer(device, MAX_INSTANCE_COUNT),
		meshMaterialsBuffer(device, MAX_MATERIAL_COUNT),
		lightBuffer(device, MAX_LIGHT_COUNT),
		globalBuffer(device, 1)
	{
		globalBuffer.Data()->frameFlags = FRAME_FLAG_DEFAULT;
	};
	SceneGlobals const& get_SceneGlobals() { return *globalBuffer.Data(); }
	RHI::Resource* get_SceneGlobalsBuffer() { return &globalBuffer; }
	RHI::Resource* get_SceneMeshInstancesBuffer() { return &meshInstancesBuffer; }
	RHI::Resource* get_SceneMeshMaterialsBuffer() { return &meshMaterialsBuffer; }
	RHI::Resource* get_SceneLightBuffer() { return &lightBuffer; }

	bool update(Scene& scene, SceneCameraComponent& camera, FrameData&& frame, ShaderData&& shader);
};