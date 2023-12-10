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

class MeshSkinning;
class HDRIProbe;
class LTCFittedLUT;
class SceneView {
public:
private:
	BufferContainer<SceneMeshInstanceData> meshInstancesBuffer;
	BufferContainer<SceneMaterial> meshMaterialsBuffer;
	BufferContainer<SceneLight> lightBuffer;
	BufferContainer<SceneGlobals> globalBuffer;

	MeshSkinning* meshSkinning;

	std::unordered_map<entt::entity, size_t> viewedComponentVersions;
public:
	struct ShaderData {
		struct IBLProbeData {
			bool use = true;
			HDRIProbe* probe = nullptr;

			float diffuseIntensity;
			float specularIntensity;
			float occlusionStrength;
			float skyboxLod;
			float skyboxIntensity;
		} probe;
		struct LTCData {
			LTCFittedLUT* ltcTable;
		} ltc;
		struct SilhouetteParams {
			float edgeThreshold;
			float3 edgeColor;
		} silhouette;
	} shaderData;
	struct FrameData {
		uint viewportWidth;
		uint viewportHeight;
		uint frameIndex;
		uint backBufferIndex;
		float frameTime;
	} frameData;
	SceneView(const SceneView&) = delete;
	SceneView(SceneView&&) = default;
	SceneView(RHI::Device* device);
	~SceneView();

	inline SceneGlobals const* get_editor_globals_data() { return globalBuffer.Data(); }
	inline RHI::Resource* get_editor_globals_buffer() { return &globalBuffer; }

	inline FrameData const& get_frame_data() { return frameData; }
	inline ShaderData const& get_shader_data() { return shaderData; }
	bool update(RHI::CommandList* ctx, Scene& scene, SceneCameraComponent& camera, FrameData&& frame, ShaderData&& shader);
};