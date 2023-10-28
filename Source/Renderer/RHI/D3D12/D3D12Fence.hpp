#pragma once
#include "../Common.hpp"
namespace RHI {
	class Device;
	class CommandQueue;
	class Fence {
		ComPtr<ID3D12Fence> m_Fence;
		HANDLE m_FenceEvent{ nullptr };
	public:
		Fence(Device* device);
		~Fence();
		inline operator ID3D12Fence* () { return m_Fence.Get(); }

		inline bool IsCompleted(size_t value) { return GetCompletedValue() >= value; }
		inline size_t GetCompletedValue() { return m_Fence->GetCompletedValue(); }
		/* GPU Side fencing */
		inline void Signal(size_t value) { m_Fence->Signal(value); }
		void Wait(size_t value);
	};
	class MarkerFence : public Fence {
		size_t nMarker{ 0 };
	public:		
		using Fence::Fence;
		void MarkAndWait(CommandQueue* queue);
	};
}