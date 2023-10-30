#include "GeometryManager.hpp"
namespace RHI {
	GeometryManager::GeometryManager(Device* device) {
		m_HandleQueue.setup(MAX_NUM_GEOS);
		m_GeoBuffers.resize(MAX_NUM_GEOS);
		m_GeometryHandles.resize(MAX_NUM_GEOS);
		m_GeoBufferSRVs.resize(MAX_NUM_GEOS);
		m_GeometryHandleBuffer = std::make_unique<Buffer>(
			device, Buffer::BufferDesc::GetGenericBufferDesc(
				::size_in_bytes(m_GeometryHandles),
				sizeof(GeometryGPUHandle),
				ResourceState::Common,
				ResourceUsage::Upload
			)
		);
		m_GeometryHandleBuffer->SetName(L"Geometry Handles");
	}
	/* Updates a section of the GeometryGPUHandleBuffer */
	void GeometryManager::SyncGeometryHandleBuffer(handle_type handle) {
		m_GeometryHandleBuffer->Update(
			&m_GeometryHandles[handle], sizeof(GeometryGPUHandle), sizeof(GeometryGPUHandle) * handle
		);
	}
	void GeometryManager::LoadMeshes(Device* device, CommandList* cmdList, const scene_t* scene) {
		auto cb = [&](StaticMesh& mesh) {
			std::vector<MeshletTriangle> swizzled_meshlet_triangles;
			mesh.swizzle_meshlet_triangles(swizzled_meshlet_triangles);
			// in the shader this can be unpacked via
			// uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
			size_t alloc_size = ::size_in_bytes(mesh.position) +
				::size_in_bytes(mesh.normal) +
				::size_in_bytes(mesh.tangent) +
				::size_in_bytes(mesh.uv) +
				::size_in_bytes(mesh.indices) +
				::size_in_bytes(mesh.meshlets) +
				::size_in_bytes(mesh.meshlet_bounds) +
				::size_in_bytes(mesh.meshlet_vertices) +
				::size_in_bytes(swizzled_meshlet_triangles);
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
			GeometryGPUHandle geo{};

			size_t offset = 0;
			auto upload = [&](auto data) {
				auto size = ::size_in_bytes(data);
				intermediate->Update(data.data(), size, offset);
				offset += size;				
			};

			geo.indices_offset = offset;
			geo.indices_count = mesh.indices.size();
			upload(mesh.indices);

			geo.position_offset = offset;
			geo.position_count = mesh.position.size();
			upload(mesh.position);

			geo.normal_offset = offset;
			geo.normal_count = mesh.normal.size();
			upload(mesh.normal);

			geo.tangent_offset = offset;
			geo.tangent_count = mesh.tangent.size();
			upload(mesh.tangent);

			geo.uv_offset = offset;
			geo.uv_count = mesh.uv.size();
			upload(mesh.uv);

			geo.meshlet_count = mesh.meshlets.size();

			geo.meshlet_offset = offset;
			upload(mesh.meshlets);

			geo.meshlet_bound_offset = offset;
			upload(mesh.meshlet_bounds);

			geo.meshlet_vertices_offset = offset;
			upload(mesh.meshlet_vertices);

			geo.meshlet_triangles_offset = offset;
			upload(swizzled_meshlet_triangles); // ! Don't upload the u8 version

			// TODO
			// blendWeights
			// blendIndices
			
			handle_type handle = m_HandleQueue.pop();
			geo.manager_handle = handle;

			// Schedule a copy
			buffer->QueueCopy(cmdList, intermediate.get(), 0, 0, alloc_size);
			buffer->SetName(std::format(L"Geo Buffer #{}", handle));
			
			m_GeoBufferSRVs[handle] = device->GetBufferShaderResourceView(buffer.get());
			geo.heap_handle = m_GeoBufferSRVs[handle].heap_handle;

			m_GeometryHandles[handle] = std::move(geo);
			m_GeoBuffers[handle] = std::move(buffer);			

			SyncGeometryHandleBuffer(handle);
		};
		IO::load_static_meshes(scene, cb);
	}
}