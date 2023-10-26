#include "D3D12CommandList.hpp"
#include "D3D12Device.hpp"
namespace RHI {
	CommandList::CommandList(Device* gpuDevice, CommandListType type, UINT numAllocators) {
		auto device = gpuDevice->operator ID3D12Device * ();
		DCHECK(numAllocators > 0);
		m_CommandAllocators.resize(numAllocators);
		for (UINT i = 0; i < numAllocators; i++) {
			CHECK_HR(device->CreateCommandAllocator((D3D12_COMMAND_LIST_TYPE)type, IID_PPV_ARGS(&m_CommandAllocators[i])));
		}
		//
		CHECK_HR(device->CreateCommandList(0, (D3D12_COMMAND_LIST_TYPE)type, m_CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList)));
		// Closed by default
		End();
	}
}