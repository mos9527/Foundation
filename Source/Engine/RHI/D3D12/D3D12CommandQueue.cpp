#include "D3D12CommandQueue.hpp"
#include "D3D12CommandList.hpp"
#include "D3D12Device.hpp"
#include "D3D12Fence.hpp"
namespace RHI {
	CommandQueue::CommandQueue(Device* device, CommandListType type) : DeviceChild(device), m_Type(type) {
		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.Type = CommandListTypeToD3DType(type);
		CHECK_HR(device->GetNativeDevice()->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue)));
		m_Fence = std::make_unique<Fence>(device);		
	}
	SyncFence CommandQueue::Execute(CommandList* cmdList) {
		CHECK(!cmdList->IsOpen());
		SyncFence fence(m_Fence.get(), GetUniqueFenceValue());
		ID3D12CommandList* ppCommandLists[] = { *cmdList };
		m_CommandQueue->ExecuteCommandLists(1, ppCommandLists);
		Signal(fence);
		return fence;
	}
	SyncFence CommandQueue::Execute(std::vector<CommandList*> cmdLists) {
		SyncFence fence(m_Fence.get(), GetUniqueFenceValue());
		ID3D12CommandList** ppCommandLists = new ID3D12CommandList * [cmdLists.size()];
		for (size_t i = 0; i < cmdLists.size(); i++) ppCommandLists[i] = *cmdLists[i];
		m_CommandQueue->ExecuteCommandLists((UINT)cmdLists.size(), ppCommandLists);
		delete[] ppCommandLists;
		Signal(fence);
		return fence;
	}
}