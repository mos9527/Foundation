#include "GeometryManager.hpp"
#include "Structs.hpp"
using namespace RHI;
using namespace IO;

static void SwizzleMeshletTriangles(std::vector<unsigned char> const& inTris, std::vector<meshlet_triangles_swizzled>& outTris) {
	for (size_t i = 0; i < inTris.size() - 3; i += 3) {
		constexpr meshlet_triangles_swizzled b10 = 0b1111111111;
		outTris.push_back(((b10 & inTris[i]) << 20) | ((b10 & inTris[i + 1]) << 10) | ((b10 & inTris[i + 2])));
	}
}
static void SwizzleMeshletBounds(std::vector<meshlet_bound> const& inBouds, std::vector<meshlet_bounds_swizzled>& outBounds) {
	for (size_t i = 0; i < inBouds.size(); i++) {
		meshlet_bounds_swizzled swizzled;
		swizzled.center[0] = inBouds[i].center[0];
		swizzled.center[1] = inBouds[i].center[1];
		swizzled.center[2] = inBouds[i].center[2];
		swizzled.radius = inBouds[i].radius;

		swizzled.cone_apex[0] = inBouds[i].cone_apex[0];
		swizzled.cone_apex[1] = inBouds[i].cone_apex[1];
		swizzled.cone_apex[2] = inBouds[i].cone_apex[2];

		swizzled.cone_axis[0] = inBouds[i].cone_axis[0];
		swizzled.cone_axis[1] = inBouds[i].cone_axis[1];
		swizzled.cone_axis[2] = inBouds[i].cone_axis[2];
		swizzled.cone_cutoff = inBouds[i].cone_cutoff;

		outBounds.push_back(swizzled);
	}
}
entt::entity GeometryManager::LoadMesh(Device* device, CommandList* cmdList, RHI::DescriptorHeap* destHeap, mesh_static const& mesh) {
	// there's no planned support for vertex shading path
	// so the mesh indices buffer will be omitted entirely
	size_t alloc_size = ::size_in_bytes(mesh.position) +
		::size_in_bytes(mesh.normal) +
		::size_in_bytes(mesh.tangent) +
		::size_in_bytes(mesh.uv);
	for (size_t lod = 0; lod < LOD_COUNT; lod++) {
		alloc_size += ::size_in_bytes(mesh.lods[lod].meshlets); // float2
		alloc_size += mesh.lods[lod].meshlet_bounds.size() * sizeof(meshlet_bounds_swizzled);
		alloc_size += (mesh.lods[lod].meshlet_triangles.size() / 3) * sizeof(meshlet_triangles_swizzled); // u8 * 3 -> u32
		alloc_size += ::size_in_bytes(mesh.lods[lod].meshlet_vertices); // UINT
	}
	// Create the buffer that GPU reads
	auto buffer = std::make_unique<Buffer>(device, Buffer::BufferDesc::GetGenericBufferDesc(
		alloc_size,
		RAW_BUFFER_STRIDE,
		ResourceState::CopyDest,
		ResourceUsage::Default
	));
	// Create the intermediate buffer
	auto desc = Buffer::BufferDesc::GetGenericBufferDesc(alloc_size);
	desc.poolType = ResourcePoolType::Intermediate;
	auto intermediate = device->AllocateIntermediateBuffer(desc);
	// Load the data onto the intermediate buffer
	GeometryData geo{};

	size_t offset = 0;
	auto upload = [&](auto data) {
		auto size = ::size_in_bytes(data);
		intermediate->Update(data.data(), size, offset);
		return size;
	};
	/* vertices */
	geo.position_offset = offset;
	geo.position_count = mesh.position.size();
	offset += upload(mesh.position);

	geo.normal_offset = offset;
	geo.normal_count = mesh.normal.size();
	offset += upload(mesh.normal);

	geo.tangent_offset = offset;
	geo.tangent_count = mesh.tangent.size();
	offset += upload(mesh.tangent);

	geo.uv_offset = offset;
	geo.uv_count = mesh.uv.size();
	offset += upload(mesh.uv);
	/* LODs */
	std::vector<meshlet_bounds_swizzled> swizzled_bounds_buffer;
	std::vector<meshlet_triangles_swizzled> swizzled_tris_buffer;
	for (size_t lod = 0; lod < LOD_COUNT; lod++) {
		swizzled_bounds_buffer.clear();
		swizzled_tris_buffer.clear();
		auto& mesh_lod = mesh.lods[lod];
		SwizzleMeshletTriangles(mesh_lod.meshlet_triangles, swizzled_tris_buffer);
		SwizzleMeshletBounds(mesh_lod.meshlet_bounds, swizzled_bounds_buffer);
		std::vector<IO::meshlet> lod_meshlet = mesh_lod.meshlets;
		// meshlet u8 triangle offset -> u32 offset
		{
			size_t tri_offset = 0;
			for (auto& meshlet : lod_meshlet) {
				meshlet.triangle_offset = tri_offset;
				tri_offset += meshlet.triangle_count;
			}
		}
		auto& handle_lod = geo.lods[lod];
		handle_lod.meshlet_count = lod_meshlet.size();

		handle_lod.meshlet_vertices_offset = offset;
		offset += upload(mesh_lod.meshlet_vertices);

		handle_lod.meshlet_offset = offset;
		offset += upload(lod_meshlet);

		handle_lod.meshlet_triangles_offset = offset;
		offset += upload(mesh_lod.meshlet_triangles);

		handle_lod.meshlet_bound_offset = offset;
		offset += upload(mesh_lod.meshlet_bounds);
	}

	// Schedule a copy
	buffer->QueueCopy(cmdList, intermediate.get(), 0, 0, alloc_size);
	Geometry geometry{
		.name = "Geometry",
		.geometry_buffer = std::move(buffer),		
	};
	DirectX::BoundingBox::CreateFromPoints(
		geometry.aabb,
		mesh.position.size(),
		mesh.position.data(),
		sizeof(XMFLOAT3)
	);
	geometry.geometry_properties = geo;
	geometry.geometry_descriptor = destHeap->AllocateDescriptor();
	// create a raw SRV for the geo buffer
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.StructureByteStride = 0;
	srvDesc.Buffer.NumElements = alloc_size / sizeof(float);
	device->GetNativeDevice()->CreateShaderResourceView(
		geometry.geometry_buffer->GetNativeBuffer(),
		&srvDesc,
		geometry.geometry_descriptor->get_cpu_handle()
	);
	// place it on the registry
	auto entity = registry.create();
	registry.emplace<Geometry>(entity, std::move(geometry));
	registry.emplace<Tag>(entity, Tag::Geometry);
	return entity;
}