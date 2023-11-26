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
		struct SubresourceTransitionBarrier : public D3D12_RESOURCE_BARRIER {
			SubresourceTransitionBarrier(const D3D12_RESOURCE_BARRIER& o) : D3D12_RESOURCE_BARRIER(o) {};
			friend inline bool operator< (const SubresourceTransitionBarrier& lhs, const SubresourceTransitionBarrier& rhs) {
				if (lhs.Transition.pResource != rhs.Transition.pResource) return lhs.Transition.pResource < rhs.Transition.pResource;
				return lhs.Transition.Subresource < rhs.Transition.Subresource;
			}
		};
		std::set<SubresourceTransitionBarrier> m_QueuedBarriers;
		std::vector<SubresourceTransitionBarrier> m_FinalBarriers;
		bool m_Closed = false;
	public:

		CommandList(Device* device, CommandListType type, uint numAllocators = 1);
		~CommandList() = default;
		inline void ResetAllocator(uint allocatorIndex=0) {
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandAllocators[allocatorIndex]->Reset());
		};
		inline void Begin(uint allocatorIndex = 0) {
			CHECK(m_Closed && "Attempting to start an already recording CommandList");
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandList->Reset(m_CommandAllocators[allocatorIndex].Get(), nullptr));
			m_Closed = false;
		};
		inline void Close() { FlushBarriers(); CHECK_HR(m_CommandList->Close()); m_Closed = true; };
		inline bool IsOpen() const { return !m_Closed; }

		void Barrier(Resource* res, ResourceState state, const uint* subresources, uint numSubresources);
		void Barrier(Resource* res, ResourceState state, uint subresource) { Barrier(res, state, &subresource, 1); }
		void Barrier(Resource* res, ResourceState state);
		inline void FlushBarriers() { 
			if (m_QueuedBarriers.size()) {
				m_FinalBarriers.clear();
				for (auto& barrier : m_QueuedBarriers) {
					if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) m_FinalBarriers.push_back(barrier);
				}
				if (m_FinalBarriers.size())
					m_CommandList->ResourceBarrier(m_FinalBarriers.size(), m_FinalBarriers.data());
				m_QueuedBarriers.clear(); 
			}
		};

		void CopyBufferRegion(Resource* src, Resource* dst, size_t srcOffsetInBytes, size_t dstOffsetInBytes, size_t sizeInBytes);
		void ZeroBufferRegion(Resource* dst, size_t dstOffsetInBytes, size_t sizeInBytes);
		void CopySubresource(Resource* src, uint srcSubresource, Resource* dst, uint dstSubresource);

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