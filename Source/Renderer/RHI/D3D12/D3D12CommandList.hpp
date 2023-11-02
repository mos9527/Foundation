#pragma once
#include "../Common.hpp"
#include "D3D12Types.hpp"
namespace RHI {
	class Device;
	class CommandList : RHIObject
	{
		using RHIObject::m_Name;
		CommandListType m_Type;
		ComPtr<ID3D12GraphicsCommandList6> m_CommandList;
		std::vector<ComPtr<ID3D12CommandAllocator>> m_CommandAllocators;
	public:

		CommandList(Device* gpuDevice, CommandListType type, uint numAllocators = 1);
		~CommandList() = default;
		inline void ResetAllocator(uint allocatorIndex) {
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandAllocators[allocatorIndex]->Reset());
		};
		inline void Begin(uint allocatorIndex = 0) {
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandList->Reset(m_CommandAllocators[allocatorIndex].Get(), nullptr));
		};
		inline void End() { CHECK_HR(m_CommandList->Close()); };
		inline void Execute(CommandList* bundle) { m_CommandList->ExecuteBundle(*bundle); }
		inline auto GetNativeCommandList() { return m_CommandList.Get(); }
		operator ID3D12GraphicsCommandList6* () { return m_CommandList.Get(); }

		using RHIObject::GetName;
		inline void SetName(name_t name) { 
			m_Name = name; 
			m_CommandList->SetName((const wchar_t*)name.c_str()); 
		}
	};
}