#pragma once
#include "../Common.hpp"
#include "D3D12CommandList.hpp"
#include "D3D12Fence.hpp"
namespace RHI {
	class Device;
	class CommandQueue {
		name_t m_Name;
		ComPtr<ID3D12CommandQueue> m_CommandQueue;
	public:
		CommandQueue(Device* device);
		~CommandQueue() = default;
		inline void SetName(name_t name) { m_Name = name; m_CommandQueue->SetName((const wchar_t*)name.c_str()); }
		void Execute(CommandList*);
		void Execute(std::vector<CommandList*>);
		inline operator ID3D12CommandQueue* () { return m_CommandQueue.Get(); }
		inline void Signal(Fence* fence, size_t value) { m_CommandQueue->Signal(*fence, value); }
		void Wait(Fence* fence, size_t value) { m_CommandQueue->Wait(*fence, value); }
	};
}