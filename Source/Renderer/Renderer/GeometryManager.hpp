#pragma once
#include "../Common.hpp"
#include "../RHI/RHI.hpp"
#include "../../IO/Mesh.hpp"
#include "Structs.hpp"
struct Geometry {
	std::string name;
	std::unique_ptr<RHI::Buffer> geometry_buffer;
	std::unique_ptr<RHI::Descriptor> geometry_descriptor;

	DirectX::BoundingBox aabb;
	GeometryData geometry_properties;
};
class GeometryManager {
public:
	GeometryManager(entt::registry& _registry) : registry(_registry) {};
	entt::entity LoadMesh(RHI::Device* device, RHI::CommandList* cmdList, RHI::DescriptorHeap* destHeap, IO::mesh_static const& mesh);

private:
	entt::registry& registry;
};