#pragma once
#include "../Common.hpp"
namespace RHI {
	class Device;
	class CommandQueue;
	class Fence : public RHIObject {
	protected:
		using RHIObject::m_Name;
		ComPtr<ID3D12Fence> m_Fence;
		HANDLE m_FenceEvent{ nullptr };
	public:
		Fence(Device* device);
		~Fence();

		inline bool IsCompleted(size_t value) { return GetCompletedValue() == value; }
		inline size_t GetCompletedValue() { return m_Fence->GetCompletedValue(); }
		/* CPU Side fencing */
		inline void Signal(size_t value) { m_Fence->Signal(value); }
		void Wait(size_t value);

		inline operator ID3D12Fence* () { return m_Fence.Get(); }

		using RHIObject::GetName;
		inline void SetName(name_t name) { 
			m_Name = name; 
			m_Fence->SetName((const wchar_t*)name.c_str()); 
		}
	};
}