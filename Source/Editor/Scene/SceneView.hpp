#pragma once
#include "../Context.hpp"
#include "../Scene/SceneComponent/SkinnedMesh.hpp"
#include "../../Runtime/Asset/ResourceContainer.hpp"
#include "../Shaders/Shared.h"
#include "../Processor/MeshSkinning.hpp"

class SceneView {
	MeshSkinning skinnedMeshVertexBuffer;
	// Default
	RHI::Buffer sceneBuffer;
	std::unique_ptr<RHI::ShaderResourceView> sceneBufferSrv;
	// Upload
	BufferContainer<SceneMeshInstanceData> meshInstancesBuffer;
	BufferContainer<SceneMaterial> materialsBuffer;
	BufferContainer<SceneLight> lightsBuffer;
	// Constants (Upload)
	BufferContainer<SceneGlobals> constGlobalsBuffer;
	BufferContainer<ShadingConstants> constShadingBuffer;

	// Versioning
	std::unordered_map<entt::entity, size_t> viewingSceneComponentVersions, viewingAssetComponentVersions;
	template<IsSceneComponent T> bool ValidateVersion(T& component) {
		if (viewingSceneComponentVersions.contains(component.get_entity()) && viewingSceneComponentVersions.at(component.get_entity()) == component.get_version())
			return false;
		return true;
	}
	template<IsAssetComponent T> bool ValidateVersion(T& component) {
		if (viewingAssetComponentVersions.contains(component.get_entity()) && viewingAssetComponentVersions.at(component.get_entity()) == component.get_version())
			return false;
		return true;
	}
	template<IsSceneComponent T> void UpdateVersion(T& component) {
		viewingSceneComponentVersions[component.get_entity()] = component.get_version();
	}
	template<IsAssetComponent T> void UpdateVersion(T& component) {
		viewingAssetComponentVersions[component.get_entity()] = component.get_version();
	}
public:
	SceneView(RHI::Device* device);
	// 0 -- numStatic -- numStatic + numSkinned -- numStatic + numSkinned + ...
	inline static SceneComponent* DecodeMeshInstanceIndex(Scene* scene, uint index) {
		auto const& skinnedMeshStorage = scene->storage<SceneSkinnedMeshComponent>();
		auto const& staticMeshStorage = scene->storage<SceneStaticMeshComponent>();
		if (index >= staticMeshStorage.size() + 0) {
			return scene->get_base<SceneComponent>(skinnedMeshStorage.at(index - staticMeshStorage.size()));
		}
		if (index >= 0) {
			return scene->get_base<SceneComponent>(staticMeshStorage.at(index));
		}
		CHECK(false && "Instance not found");
	}
	inline static uint EncodeMeshInstanceIndex(SceneComponent* mesh) {
		auto const& skinnedMeshStorage = mesh->parent.storage<SceneSkinnedMeshComponent>();
		auto const& staticMeshStorage = mesh->parent.storage<SceneStaticMeshComponent>();
		if (mesh->get_type() == SceneComponentType::SkinnedMesh) {
			return staticMeshStorage.size() + skinnedMeshStorage.index(mesh->get_entity());
		}
		if (mesh->get_type() == SceneComponentType::StaticMesh) {
			return staticMeshStorage.index(mesh->get_entity());
		}
		CHECK(false && "Invalid component. Must be SceneStaticMeshComponent / SceneSkinnedMeshComponent");
		return -1;
	};
	void Update(RHI::CommandList* ctx, RHIContext* rhiCtx, SceneContext* sceneCtx, EditorContext* editorCtx);
	auto& GetGlobalsBuffer() { return constGlobalsBuffer; }
	auto& GetShadingBuffer() { return constShadingBuffer; }
};
