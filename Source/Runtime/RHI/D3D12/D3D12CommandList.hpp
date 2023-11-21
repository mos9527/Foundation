#pragma once
#include "D3D12Types.hpp"
#include "D3D12Fence.hpp"
namespace RHI {	
	class Resource;
	class CommandList : public DeviceChild
	{
		name_t m_Name;
		CommandListType m_Type;
		ComPtr<ID3D12GraphicsCommandList6> m_CommandList;
		std::vector<ComPtr<ID3D12CommandAllocator>> m_CommandAllocators;
		bool m_HasBegun = false;
	public:

		CommandList(Device* device, CommandListType type, uint numAllocators = 1);
		~CommandList() = default;
		inline void ResetAllocator(uint allocatorIndex=0) {
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandAllocators[allocatorIndex]->Reset());
		};
		inline void Begin(uint allocatorIndex = 0) {
			CHECK(!m_HasBegun && "Attempting to start an already recording CommandList");
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandList->Reset(m_CommandAllocators[allocatorIndex].Get(), nullptr));
			m_HasBegun = true;
		};
		inline void End() { CHECK_HR(m_CommandList->Close()); m_HasBegun = false; };
		inline bool IsOpen() const { return m_HasBegun; }

		void CopyBufferRegion(Resource* src, Resource* dst, size_t srcOffset, size_t dstOffset, size_t size);
		void ZeroBufferRegion(Resource* dst, size_t dstOffset, size_t size);
		void CopySubresource(Resource* src, uint srcSsubresource, Resource* dst, uint dstSubresource);
		SyncFence Execute();
		CommandQueue* GetCommandQueue();
		inline void ExecuteBundle(CommandList* bundle) { m_CommandList->ExecuteBundle(*bundle); }
		inline auto GetNativeCommandList() { return m_CommandList.Get(); }
		operator ID3D12GraphicsCommandList6* () { return m_CommandList.Get(); }

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;
			m_CommandList->SetName(name);
		}
	};
}