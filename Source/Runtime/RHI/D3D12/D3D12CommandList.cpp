#include "D3D12CommandList.hpp"
#include "D3D12Resource.hpp"
#include "D3D12Device.hpp"
namespace RHI {
	CommandList::CommandList(Device* device, CommandListType type, uint numAllocators) : DeviceChild(device), m_Type(type) {
		DCHECK(numAllocators > 0);
		m_CommandAllocators.resize(numAllocators);
		for (uint i = 0; i < numAllocators; i++) {
			CHECK_HR(device->GetNativeDevice()->CreateCommandAllocator(CommandListTypeToD3DType(type), IID_PPV_ARGS(&m_CommandAllocators[i])));
		}
		CHECK_HR(device->GetNativeDevice()->CreateCommandList(0, CommandListTypeToD3DType(type), m_CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList)));
		Close(); // Start closed
	}
	void CommandList::CopyBufferRegion(Resource* src, Resource* dst, size_t srcOffset, size_t dstOffset, size_t size) {
		CHECK(dst->GetSubresourceState() == ResourceState::CopyDest);
		GetNativeCommandList()->CopyBufferRegion(*dst, dstOffset, *src, srcOffset, size);
	};
	void CommandList::ZeroBufferRegion(Resource* dst, size_t dstOffset, size_t size) {
		CHECK(dst->GetSubresourceState() == ResourceState::CopyDest);
		const size_t zeroBlock = m_Device->GetZeroBuffer()->GetDesc().sizeInBytes();
		size_t written = 0;
		while (written < size) {
			size_t toWrite = std::min(zeroBlock, size - written);
			CopyBufferRegion(m_Device->GetZeroBuffer(), dst, 0, dstOffset, toWrite);
			written += toWrite;
		}
	}
	void CommandList::CopySubresource(Resource* src, uint srcSsubresource, Resource* dst, uint dstSubresource) {
		CHECK(src->GetSubresourceState() == ResourceState::CopySource);
		CHECK(dst->GetSubresourceState() == ResourceState::CopyDest);
		D3D12_TEXTURE_COPY_LOCATION srcLocation, dstLocation;
		srcLocation.pResource = *src;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLocation.SubresourceIndex = srcSsubresource;
		dstLocation.pResource = *dst;
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = dstSubresource;
		m_CommandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
	}
	void CommandList::Barrier(Resource* res, ResourceState state, const uint* subresources, uint numSubresources) {
		for (uint i = 0; i < numSubresources; i++) {
			uint subresource = *(subresources + i);
			if (res->m_States[subresource] != state) {
				m_QueuedBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
					res->GetNativeResource(),
					(D3D12_RESOURCE_STATES)res->GetSubresourceState(subresource),
					(D3D12_RESOURCE_STATES)state,
					subresource
				));
				res->m_States[subresource] = state;
			}
		}
	}
	void CommandList::Barrier(Resource* res, ResourceState state) {
		Barrier(res, state, res->m_SubresourceRange.data(), res->m_SubresourceRange.size());
	}
	CommandQueue* CommandList::GetCommandQueue() { return m_Device->GetCommandQueue(m_Type); }
	SyncFence CommandList::Execute() {
		FlushBarriers();
		return GetCommandQueue()->Execute(this);
	}
}