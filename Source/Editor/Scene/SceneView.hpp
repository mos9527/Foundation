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

#include "../Processor/StaticMeshBufferProcessor.hpp"
#include "../Processor/SkinnedMeshBufferProcessor.hpp"
#include "../Processor/IBLProbeProcessor.hpp"
#include "../Processor/LTCTableProcessor.hpp"
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

			SceneIBLProbe to_hlsl() const {
				SceneIBLProbe hlsl{};
				hlsl.enabled = use;
				hlsl.cubemapHeapIndex = probe->cubemapSRV->descriptor.get_heap_handle();
				hlsl.radianceHeapIndex = probe->radianceCubeArraySRV->descriptor.get_heap_handle();
				hlsl.irradianceHeapIndex = probe->irridanceCubeSRV->descriptor.get_heap_handle();
				hlsl.lutHeapIndex = probe->lutArraySRV->descriptor.get_heap_handle();
				hlsl.mips = probe->numMips;
				hlsl.diffuseIntensity = diffuseIntensity;
				hlsl.specularIntensity = specularIntensity;
				hlsl.occlusionStrength = occlusionStrength;
				return hlsl;
			}
		} probe;
		struct LTCData {
			LTCTableProcessor* ltcTable;			
			const uint get_heap_handle() const {
				return ltcTable ? ltcTable->ltcSRV->descriptor.get_heap_handle() : INVALID_HEAP_HANDLE;
			}
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
	{};

	inline SceneGlobals const* get_editor_globals_data() { return globalBuffer.Data(); }
	inline RHI::Resource* get_editor_globals_buffer() { return &globalBuffer; }

	inline RHI::Resource* get_meshbuffer_static() { return staticMeshBuffers.sceneMeshBuffer.get(); }
	inline RHI::Resource* get_meshbuffer_skinned() { return skinnedMeshBuffers.sceneMeshBuffer.get(); }

	inline FrameData const& get_frame_data() { return frameData; }
	inline ShaderData const& get_shader_data() { return shaderData; }
	bool update(RHI::CommandList* ctx, Scene& scene, SceneCameraComponent& camera, FrameData&& frame, ShaderData&& shader);
};