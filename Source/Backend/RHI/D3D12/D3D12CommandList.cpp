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
	SyncFence CommandList::Execute() { 
		return m_Device->GetCommandQueue(m_Type)->Execute(this); 
	}
}