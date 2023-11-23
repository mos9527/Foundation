#pragma once
#include "../RHI/RHI.hpp"
class UploadContext : public RHI::CommandList {
	std::vector<std::unique_ptr<RHI::Resource>> m_IntermediateBuffers;
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> m_FootprintBuffer;
	std::vector<UINT64> m_RowSizesBuffer;
	std::vector<UINT> m_RowsBuffer;
	void PrepareTempBuffers(uint numSubresource) {
		m_FootprintBuffer.resize(numSubresource);
		m_RowSizesBuffer.resize(numSubresource);
		m_RowsBuffer.resize(numSubresource);
	}
	std::mutex m_UploadMutex;
	RHI::SyncFence m_CopyFence;
public:
	UploadContext(RHI::Device* device) : RHI::CommandList(device, RHI::CommandListType::Copy, 1) {};
	// Initiates the command list and cleans previous intermediate buffer resources
	using RHI::CommandList::Begin;
	void QueueUpload(RHI::Resource* dst, RHI::Subresource* data, uint firstSubresource = 0, uint subresourceCount = 1);
	void QueueUpload(RHI::Resource* dst, void* data, uint sizeInBytes);
	using RHI::CommandList::CopyBufferRegion;
	using RHI::CommandList::CopySubresource;
	RHI::SyncFence End();
	void Flush() {
		CHECK(m_CopyFence.IsCompleted() && "Attempting to flush a in-flight UploadContext");
		m_IntermediateBuffers.clear();
	}
	~UploadContext() {
		if (m_CopyFence.Valid())
			m_CopyFence.Wait();
	}
};
