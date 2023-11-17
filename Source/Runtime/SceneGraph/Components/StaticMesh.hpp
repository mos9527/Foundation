#pragma once
#include "../Component.hpp"
#include "../../AssetRegistry/IO.hpp"
#include "../../AssetRegistry/AssetRegistry.hpp"

struct StaticMeshComponent : public SceneComponent {
	asset_handle mesh_resource;
};

template<> struct SceneComponentTraits<StaticMeshComponent> {
	static constexpr SceneComponentType type = SceneComponentType::StaticMesh;
};