#pragma once
#include "../Common.hpp"
#include "D3D12CommandList.hpp"
#include "D3D12Fence.hpp"
namespace RHI {
	class Device;
	class CommandQueue {
		name_t m_Name;
		CommandListType m_Type;
		ComPtr<ID3D12CommandQueue> m_CommandQueue;
		std::atomic<size_t> m_FenceValue;
	public:
		CommandQueue(Device* device, CommandListType type = CommandListType::Direct);
		~CommandQueue() = default;
		inline void SetName(name_t name) { m_Name = name; m_CommandQueue->SetName((const wchar_t*)name.c_str()); }
		void Execute(CommandList*);
		void Execute(std::vector<CommandList*>);
		inline operator ID3D12CommandQueue* () { return m_CommandQueue.Get(); }
		inline void Signal(Fence* fence, size_t value) { m_CommandQueue->Signal(*fence, value); }
		inline void Wait(Fence* fence, size_t value) { m_CommandQueue->Wait(*fence, value); }
		inline size_t GetUniqueFenceValue() { return m_FenceValue++; }
	};
}