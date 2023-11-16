#pragma once
#include "D3D12Fence.hpp"
namespace RHI {
	class Device;
	class CommandList;
	class CommandQueue : public DeviceChild {
		name_t m_Name;
		CommandListType m_Type;
		ComPtr<ID3D12CommandQueue> m_CommandQueue;
		std::unique_ptr<Fence> m_Fence;		
		std::atomic<size_t> m_FenceValue = 1;
	public:
		CommandQueue(Device* device, CommandListType type = CommandListType::Direct);
		~CommandQueue() = default;
		SyncFence Execute(CommandList*);
		SyncFence Execute(std::vector<CommandList*>);
		inline operator ID3D12CommandQueue* () { return m_CommandQueue.Get(); }
		inline void Signal(Fence* fence, size_t value) { m_CommandQueue->Signal(*fence, value); }
		inline void Signal(SyncFence sfence) { Signal(sfence.fence, sfence.value); }
		inline void Wait(Fence* fence, size_t value) { m_CommandQueue->Wait(*fence, value); }
		inline void Wait(SyncFence sfence) { Wait(sfence.fence, sfence.value); }
		inline void Wait() { 
			auto this_value = GetUniqueFenceValue();
			Signal(m_Fence.get(), this_value);
			Wait(m_Fence.get(), this_value);
		}
		inline size_t GetUniqueFenceValue() { return m_FenceValue++; }
		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;
			m_CommandQueue->SetName((const wchar_t*)name.c_str());
		}
	};
}