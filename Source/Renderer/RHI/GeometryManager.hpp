#pragma once
#include "RHI.hpp"
#include "../../IO/Mesh.hpp"
#define MAX_NUM_GEOS 0xffff
using namespace IO;
namespace RHI {	
	struct GeometryInstance {
		XMMATRIX worldMatrix;
	};
	struct GeometryGPUHandle {
		// index into the manager
		handle manager_handle;
		// geobuffer srv index into the device descriptor heap
		handle heap_handle;
		
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
		GeometryGPULod lods[mesh_lod_num_levels];
		//// TODO
		//uint blendweights_offset;
		//uint blendweights_count;
		//// TODO
		//uint blendindices_offset;
		//uint blendindices_count;
	};
	class GeometryManager {
	public:
		GeometryManager(Device* device);
		void LoadMeshes(Device* device, CommandList* cmdList, const scene_t* scene);
		inline void UnmapHandleBuffer() { m_GeometryHandleBuffer->Unmap(); }

		inline GeometryGPUHandle const& GetGeometryHandle(handle handle) { return m_GeometryHandles[handle]; }
		inline Buffer* GetGeometryBuffer(handle handle) { return m_GeoBuffers[handle].get(); }
		inline Buffer* GetGeometryHandleBuffer() { return m_GeometryHandleBuffer.get(); }
	private:
		void SyncGeometryHandleBuffer(handle index);
		handle_queue m_HandleQueue;
		
		std::vector<GeometryGPUHandle> m_GeometryHandles;
		std::unique_ptr<Buffer> m_GeometryHandleBuffer;

		std::vector<std::unique_ptr<Buffer>> m_GeoBuffers;
		std::vector<DescriptorHandle> m_GeoBufferSRVs;
	};
}