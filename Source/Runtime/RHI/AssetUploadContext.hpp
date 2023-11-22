#pragma once
#include "RHI.hpp"

// Thread safe CommandList with helpers for uploading data from CPU to GPU
class AssetUploadContext : public RHI::CommandList {
	std::vector<std::unique_ptr<RHI::Resource>> intermediateBuffers;

	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> FootprintBuffer;
	std::vector<UINT64> RowSizesBuffer;
	std::vector<UINT> RowsBuffer;
	void prepare_temp_buffers(uint numSubresource) {
		FootprintBuffer.resize(numSubresource);
		RowSizesBuffer.resize(numSubresource);
		RowsBuffer.resize(numSubresource);
	}

	std::mutex uploadMutex;
public:
	AssetUploadContext(RHI::Device* device, RHI::CommandListType type) : RHI::CommandList(device, type, 1) {};
	// Initiates the command list and cleans previous intermediate buffer resources
	void Begin() {
		RHI::CommandList::Begin(0);
		Clean();
	}
	/* Asset manipulation functions */
	void Upload(RHI::Resource* dst, RHI::Subresource* data, uint firstSubresource = 0, uint subresourceCount = 1);
	void Upload(RHI::Resource* dst, void* data, uint sizeInBytes);
	void Clean() { intermediateBuffers.clear(); }
};
