#pragma once
#include "Common.hpp"
#include "../RHI/RHI.hpp"
#include "../../IO/Mesh.hpp"
struct GeometryGPUHandle {
	// index into the manager
	uint manager_handle;
	// geobuffer srv index into the device descriptor heap
	uint heap_handle;

	/* these are relative to the big geo buffers unique per geo */
	uint position_offset;
	uint position_count;

	uint normal_offset;
	uint normal_count;

	uint tangent_offset;
	uint tangent_count;

	uint uv_offset;
	uint uv_count;

	struct GeometryGPULod {
		uint meshlet_offset;
		uint meshlet_bound_offset;
		uint meshlet_vertices_offset;
		uint meshlet_triangles_offset;
		uint meshlet_count;
	};
	GeometryGPULod lods[LOD_COUNT];
	//// TODO
	//uint blendweights_offset;
	//uint blendweights_count;
	//// TODO
	//uint blendindices_offset;
	//uint blendindices_count;
};
struct Geometry {
	std::string name;
	std::unique_ptr<RHI::Buffer> geometry_buffer;
};
class GeometryManager {
public:
	GeometryManager(entt::registry& _registery);
	entt::entity LoadMesh(RHI::Device* device, RHI::CommandList* cmdList,  IO::mesh_static const& mesh);

private:
	entt::registry& registery;
};