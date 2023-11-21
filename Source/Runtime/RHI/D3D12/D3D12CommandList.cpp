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
		//
		CHECK_HR(device->GetNativeDevice()->CreateCommandList(0, CommandListTypeToD3DType(type), m_CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList)));
		// Closed by default
		End();
	}
	void CommandList::CopyBufferRegion(Resource* src, Resource* dst, size_t srcOffset, size_t dstOffset, size_t size) {
		GetNativeCommandList()->CopyBufferRegion(*dst, dstOffset, *src, srcOffset, size);
	};
	void CommandList::ZeroBufferRegion(Resource* dst, size_t dstOffset, size_t size) {
		const size_t zeroBlock = m_Device->GetZeroBuffer()->GetDesc().sizeInBytes();
		size_t written = 0;
		while (written < size) {
			size_t toWrite = std::min(zeroBlock, size - written);
			CopyBufferRegion(m_Device->GetZeroBuffer(), dst, 0, dstOffset, toWrite);
			written += toWrite;
		}
	}
	CommandQueue* CommandList::GetCommandQueue() { return m_Device->GetCommandQueue(m_Type); }
	SyncFence CommandList::Execute() { 
		return GetCommandQueue()->Execute(this);
	}
}