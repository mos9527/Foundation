#include "Geometry.hpp"
#include "../Structs.hpp"
#include "../Renderer.hpp"
using namespace RHI;
using namespace IO;

static void swizzle_meshlet_triangles(std::vector<unsigned char> const& inTris, std::vector<meshlet_triangles_swizzled>& outTris) {
	for (size_t i = 0; i < inTris.size() - 3; i += 3) {
		constexpr meshlet_triangles_swizzled b10 = 0b1111111111;
		outTris.push_back(((b10 & inTris[i]) << 20) | ((b10 & inTris[i + 1]) << 10) | ((b10 & inTris[i + 2])));
	}
}
void Geometry::load_static_mesh(Renderer* renderer, mesh_static const& mesh) {
	allocSize = mesh.position.size() * sizeof(::vertex_static); // vertices
	for (size_t lod = 0; lod < LOD_COUNT; lod++) {
		allocSize += ::size_in_bytes(mesh.lods[lod].indices); // LOD indices (u32)
		allocSize += ::size_in_bytes(mesh.lods[lod].meshlet_vertices); // LOD meshlet vertices (u32)
		allocSize += mesh.lods[lod].meshlets.size() * sizeof(::meshlet); // LOD meshlets
		allocSize += DivRoundUp(mesh.lods[lod].meshlet_triangles.size(), 3) * sizeof(uint); // LOD meshlet triangles (3 * u8 -> u32)
	}	

	// Allocate the intermedaite buffer which we'd be currently writing to
	auto desc = Buffer::BufferDesc::GetGenericBufferDesc(allocSize);
	desc.poolType = ResourcePoolType::Intermediate;
	intermediateBuffer = renderer->allocate_buffer(desc);
	auto interemediateBufferResource = renderer->get_buffer(intermediateBuffer);
	// Load the data onto the intermediate buffer
	GeometryBufferOffsets& offsets = bufferOffsets;

	size_t offset = 0;
	auto upload = [&](auto data) {
		auto size = ::size_in_bytes(data);
		interemediateBufferResource->Update(data.data(), size, offset);
		return size;
	};
	// Vertices
	std::vector<::vertex_static> vertices(mesh.position.size());	
	for (uint i = 0; i < mesh.position.size(); i++) {
		::vertex_static& vertex = vertices[i];
		vertex.position = mesh.position[i];
		vertex.normal = mesh.normal[i];
		vertex.tangent = mesh.tangent[i];
		vertex.uv = mesh.uv[i];
	}
	offsets.vertex_offset = offset;
	offsets.vertex_count = vertices.size();
	offset += upload(vertices);
	// LODs
	for (uint lodIndex = 0; lodIndex < LOD_COUNT; lodIndex++) {
		auto& lod = offsets.lods[lodIndex];
		auto& lodSrc = mesh.lods[lodIndex];
		// indices
		lod.index_offset = offset;
		lod.index_count = lodSrc.indices.size();
		offset += upload(lodSrc.indices);

		// meshlet triangles
		std::vector<uint> meshlet_triangles;
		swizzle_meshlet_triangles(lodSrc.meshlet_triangles, meshlet_triangles);
		uint triangle_offset = offset;
		offset += upload(meshlet_triangles);

		// meshlet vertices
		uint vertex_offset = offset;
		offset += upload(lodSrc.meshlet_vertices);

		// meshlets
		std::vector<::meshlet> meshlets(lodSrc.meshlets.size());
		for (uint j = 0; j < lodSrc.meshlets.size(); j++) {
			::meshlet& current = meshlets[j];
			// vtx & primitives
			current.vertex_offset = vertex_offset;
			current.vertex_count = lodSrc.meshlet_vertices.size();
			current.triangle_offset = triangle_offset;
			current.triangle_count = meshlet_triangles.size();
			// bounds
			auto& currentBound = lodSrc.meshlet_bounds[j];
			current.center[0] = currentBound.center[0];
			current.center[1] = currentBound.center[1];
			current.center[2] = currentBound.center[2];
			current.cone_apex[0] = currentBound.cone_apex[0];
			current.cone_apex[1] = currentBound.cone_apex[1];
			current.cone_apex[2] = currentBound.cone_apex[2];
			current.cone_axis_cutoff = (currentBound.cone_axis_s8[2] << 24) | (currentBound.cone_axis_s8[1] << 16) | (currentBound.cone_axis_s8[0] << 8) | currentBound.cone_cutoff_s8;
		}		
		
		lod.meshlet_offset = offset;
		lod.meshlet_count = meshlets.size();
		offset += upload(meshlets);
	}	
}

entt::entity Geometry::upload(Renderer* renderer, RHI::CommandList* cmdList) {
	CHECK(intermediateBuffer != entt::tombstone);
	// Creates the buffer to store geometry data
	geometryBuffer = renderer->allocate_buffer(Buffer::BufferDesc::GetGenericBufferDesc(
		allocSize,
		RAW_BUFFER_STRIDE,
		ResourceState::CopyDest,
		ResourceUsage::Default
	));
	// Schedule a copy
	renderer->get_buffer(geometryBuffer)->QueueCopy(cmdList, renderer->get_buffer(intermediateBuffer), 0, 0, allocSize);
	
	return geometryBuffer;
}