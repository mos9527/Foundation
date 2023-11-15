#pragma once
#include "../Component.hpp"
#include "../../../Backend/IO/IO.hpp"
#include "../../IO/Types.hpp"
struct StaticMeshComponent : public SceneComponent {
	IO::asset_handle mesh_resource;
};