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
	void GeometryManager::SyncGeometryHandleBuffer(handle handle) {
		m_GeometryHandleBuffer->Update(
			&m_GeometryHandles[handle], sizeof(GeometryGPUHandle), sizeof(GeometryGPUHandle) * handle
		);
	}
	typedef UINT meshlet_triangles_swizzled;
	static void SwizzleMeshletTriangles(std::vector<unsigned char>& inTris, std::vector<meshlet_triangles_swizzled>& outTris) {
		for (size_t i = 0; i < inTris.size() - 3; i += 3) {
			constexpr meshlet_triangles_swizzled b10 = 0b1111111111;
			outTris.push_back(((b10 & inTris[i]) << 20) | ((b10 & inTris[i + 1]) << 10) | ((b10 & inTris[i + 2])));
		}
	}
	struct meshlet_bounds_swizzled {		
		float center[3];
		float radius;
	};
	static void SwizzleMeshletBounds(std::vector<meshlet_bound>& inBouds, std::vector<meshlet_bounds_swizzled>& outBounds) {
		for (size_t i = 0; i < inBouds.size(); i++) {
			meshlet_bounds_swizzled swizzled;
			swizzled.center[0] = inBouds[i].center[0];
			swizzled.center[1] = inBouds[i].center[1];
			swizzled.center[2] = inBouds[i].center[2];
			swizzled.radius = inBouds[i].radius;
			outBounds.push_back(swizzled);
		}
	}
	void GeometryManager::LoadMeshes(Device* device, CommandList* cmdList, const scene_t* scene) {		
		std::vector<meshlet_bounds_swizzled> swizzled_bounds_buffer;
		std::vector<meshlet_triangles_swizzled> swizzled_tris_buffer;
		auto cb = [&](mesh_static& mesh) {
			// there's no planned support for vertex shading path
			// so the mesh indices buffer will be omitted entirely
			size_t alloc_size = ::size_in_bytes(mesh.position) +
				::size_in_bytes(mesh.normal) +
				::size_in_bytes(mesh.tangent) +
				::size_in_bytes(mesh.uv);
			for (size_t lod = 0; lod < mesh_lod_num_levels; lod++) {				
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
			GeometryGPUHandle geo{};

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
			for (size_t lod = 0; lod < mesh_lod_num_levels; lod++) {
				swizzled_bounds_buffer.clear();
				swizzled_tris_buffer.clear();
				auto& mesh_lod = mesh.lods[lod];
				SwizzleMeshletTriangles(mesh_lod.meshlet_triangles, swizzled_tris_buffer);
				SwizzleMeshletBounds(mesh_lod.meshlet_bounds, swizzled_bounds_buffer);
				// meshlet u8 triangle offset -> u32 offset
				{
					size_t tri_offset = 0;
					for (auto& meshlet : mesh_lod.meshlets) {
						meshlet.triangle_offset = tri_offset;
						tri_offset += meshlet.triangle_count;
					}
				}
				auto& handle_lod = geo.lods[lod];
				handle_lod.meshlet_count = mesh_lod.meshlets.size();
				
				handle_lod.meshlet_vertices_offset = offset;
				offset += upload(mesh_lod.meshlet_vertices);

				handle_lod.meshlet_offset = offset;
				offset += upload(mesh_lod.meshlets);

				handle_lod.meshlet_triangles_offset = offset;
				offset += upload(mesh_lod.meshlet_triangles);

				handle_lod.meshlet_bound_offset = offset;
				offset += upload(mesh_lod.meshlet_bounds);
			}
			// TODO
			// blendWeights
			// blendIndices
			
			handle handle = m_HandleQueue.pop();
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