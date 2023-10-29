#include "D3D12CommandList.hpp"
#include "D3D12Device.hpp"
namespace RHI {
	static D3D12_COMMAND_LIST_TYPE GetCommandListD3DType(CommandList::CommandListType type) {
		switch (type)
		{
		case CommandList::DIRECT:
			return D3D12_COMMAND_LIST_TYPE_DIRECT;
		case CommandList::COMPUTE:
			return D3D12_COMMAND_LIST_TYPE_COMPUTE;
		case CommandList::COPY:
			return D3D12_COMMAND_LIST_TYPE_COPY;		
		default:
			return (D3D12_COMMAND_LIST_TYPE)-1;
		}
	}
	CommandList::CommandList(Device* gpuDevice, CommandListType type, uint numAllocators) {
		auto device = gpuDevice->operator ID3D12Device * ();
		DCHECK(numAllocators > 0);
		m_CommandAllocators.resize(numAllocators);
		for (uint i = 0; i < numAllocators; i++) {
			CHECK_HR(device->CreateCommandAllocator(GetCommandListD3DType(type), IID_PPV_ARGS(&m_CommandAllocators[i])));
		}
		//
		CHECK_HR(device->CreateCommandList(0, GetCommandListD3DType(type), m_CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList)));
		// Closed by default
		End();
	}
}