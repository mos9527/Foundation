#pragma once
#include "../../Runtime/RHI/RHI.hpp"
#include "../../Runtime/Asset/ResourceContainer.hpp"
#include "../Shaders/Shared.h"

#include "Scene.hpp"
#include "SceneComponent/StaticMesh.hpp"
#include "SceneComponent/SkinnedMesh.hpp"
#include "SceneComponent/Camera.hpp"
#include "SceneComponent/Light.hpp"
#include "AssetComponet/Material.hpp"
#include "AssetComponet/StaticMesh.hpp"
#include "AssetComponet/SkinnedMesh.hpp"

#include "../Processor/StaticMeshBufferProcessor.hpp"
#include "../Processor/SkinnedMeshBufferProcessor.hpp"
class IBLProbeProcessor;
class LTCTableProcessor;
class SceneView {
public:
private:
	BufferContainer<SceneMeshInstanceData> meshInstancesBuffer;
	BufferContainer<SceneMaterial> meshMaterialsBuffer;
	BufferContainer<SceneLight> lightBuffer;
	BufferContainer<SceneGlobals> globalBuffer;

	StaticMeshBufferProcessor staticMeshBuffers;
	SkinnedMeshBufferProcessor skinnedMeshBuffers;

	std::unordered_map<entt::entity, size_t> viewedComponentVersions;
public:
	struct ShaderData {
		struct IBLProbeData {
			bool use = true;
			IBLProbeProcessor* probe = nullptr;

			float diffuseIntensity;
			float specularIntensity;
			float occlusionStrength;
			float skyboxLod;
			float skyboxIntensity;
		} probe;
		LTCTableProcessor* ltcTable;
		struct SilhouetteParams {
			float  edgeThreshold;
			float3 edgeColor;
		} silhouette;
	} shaderData;
	struct FrameData {
		uint viewportWidth;
		uint viewportHeight;
		uint frameIndex;
		uint frameFlags;
		uint backBufferIndex;
		float frameTimePrev;
	} frameData;
	SceneView(const SceneView&) = delete;
	SceneView(SceneView&&) = default;
	SceneView(RHI::Device* device) :
		meshInstancesBuffer(device, MAX_INSTANCE_COUNT),
		meshMaterialsBuffer(device, MAX_MATERIAL_COUNT),
		lightBuffer(device, MAX_LIGHT_COUNT),
		globalBuffer(device, 1),
		staticMeshBuffers(device),
		skinnedMeshBuffers(device)
	{
		globalBuffer.Data()->frameFlags = FRAME_FLAG_DEFAULT;
	};
	inline SceneGlobals const& get_SceneGlobals() { return *globalBuffer.Data(); }
	inline RHI::Resource* get_SceneGlobalsBuffer() { return &globalBuffer; }
	inline RHI::Resource* get_SceneMeshInstancesBuffer() { return &meshInstancesBuffer; }
	inline RHI::Resource* get_SceneMeshMaterialsBuffer() { return &meshMaterialsBuffer; }
	inline RHI::Resource* get_SceneLightBuffer() { return &lightBuffer; }

	inline RHI::Resource* get_StaticMeshBuffer() { return staticMeshBuffers.sceneMeshBuffer.get(); }
	inline RHI::Resource* get_SkinnedMeshBuffer() { return skinnedMeshBuffers.sceneMeshBuffer.get(); }

	inline FrameData const& get_FrameData() { return frameData; }
	inline ShaderData const& get_ShaderData() { return shaderData; }
	bool update(Scene& scene, SceneCameraComponent& camera, FrameData&& frame, ShaderData&& shader);
};