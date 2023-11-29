#pragma once
#include "../RHI/RHI.hpp"
#include "ResourceContainer.hpp"

class UploadContext : RHI::CommandList {
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
	using RHI::CommandList::Begin;
	using RHI::CommandList::GetParent;
	Texture2DContainer* CreateIntermediateTexture2DContainer(RHI::Resource* dst);

	void QueueUploadTexture(RHI::Resource* dst, RHI::Resource* intermediate, uint firstSubresource = 0, uint subresourceCount = 1);
	void QueueUploadBuffer(RHI::Resource* dst, void* data, uint sizeInBytes);

	using RHI::CommandList::CopyBufferRegion;
	using RHI::CommandList::CopySubresource;
	RHI::SyncFence End();
	void Flush() {
		CHECK(!m_CopyFence.Valid() || m_CopyFence.IsCompleted() && "Attempting to flush a in-flight or UploadContext");
		m_IntermediateBuffers.clear();
	}
	~UploadContext() {
		if (m_CopyFence.Valid())
			m_CopyFence.Wait();
		Flush();
	}
};
