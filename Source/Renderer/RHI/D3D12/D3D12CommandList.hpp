#pragma once
#include "../Common.hpp"
namespace RHI {
	class Device;
	class CommandList
	{
		name_t m_Name;
		ComPtr<ID3D12GraphicsCommandList6> m_CommandList;
		std::vector<ComPtr<ID3D12CommandAllocator>> m_CommandAllocators;
	public:
		enum CommandListType
		{
			DIRECT = D3D12_COMMAND_LIST_TYPE_DIRECT,
			COMPUTE = D3D12_COMMAND_LIST_TYPE_COMPUTE,
			COPY = D3D12_COMMAND_LIST_TYPE_COPY,
			NUM_TYPES = 3
		};
		CommandList(Device* gpuDevice, CommandListType type, UINT numAllocators = 1);
		~CommandList() = default;
		inline void SetName(name_t name) { m_Name = name; m_CommandList->SetName((const wchar_t*)name.c_str()); }
		inline void ResetAllocator(UINT allocatorIndex) {
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandAllocators[allocatorIndex]->Reset());
		};
		inline void Begin(UINT allocatorIndex = 0) {
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandList->Reset(m_CommandAllocators[allocatorIndex].Get(), nullptr));
		};
		inline void End() { CHECK_HR(m_CommandList->Close()); };
		inline void Execute(CommandList* bundle) { m_CommandList->ExecuteBundle(*bundle); }
		inline auto GetNativeCommandList() { return m_CommandList.Get(); }
		operator ID3D12GraphicsCommandList6* () { return m_CommandList.Get(); }
	};
}