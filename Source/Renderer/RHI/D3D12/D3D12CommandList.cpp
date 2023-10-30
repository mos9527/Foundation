#include "D3D12CommandList.hpp"
#include "D3D12Device.hpp"
namespace RHI {
	CommandList::CommandList(Device* gpuDevice, CommandListType type, uint numAllocators) : m_Type(type) {
		auto device = gpuDevice->operator ID3D12Device * ();
		DCHECK(numAllocators > 0);
		m_CommandAllocators.resize(numAllocators);
		for (uint i = 0; i < numAllocators; i++) {
			CHECK_HR(device->CreateCommandAllocator(CommandListTypeToD3DType(type), IID_PPV_ARGS(&m_CommandAllocators[i])));
		}
		//
		CHECK_HR(device->CreateCommandList(0, CommandListTypeToD3DType(type), m_CommandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_CommandList)));
		// Closed by default
		End();
	}
}