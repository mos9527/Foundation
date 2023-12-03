#pragma once
#include "D3D12Types.hpp"
#include "D3D12Fence.hpp"
#include "D3D12Resource.hpp"
namespace RHI {	
	class Resource;
	class CommandList : public DeviceChild
	{
		name_t m_Name;
		CommandListType m_Type;
		ComPtr<ID3D12GraphicsCommandList6> m_CommandList;
		std::vector<ComPtr<ID3D12CommandAllocator>> m_CommandAllocators;
		struct TransitionBarrier : public D3D12_RESOURCE_BARRIER {
			Resource* pResourceRHI = nullptr;
			TransitionBarrier(Resource* resource, uint subresource, uint stateAfter, uint stateBefore) : D3D12_RESOURCE_BARRIER({}) {
				Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;				
				pResourceRHI = resource;
				Transition.pResource = resource->GetNativeResource();
				Transition.Subresource = subresource;
				Transition.StateAfter = (D3D12_RESOURCE_STATES)stateAfter;
				Transition.StateBefore = (D3D12_RESOURCE_STATES)stateBefore;
			}
			TransitionBarrier(const D3D12_RESOURCE_BARRIER& o) : D3D12_RESOURCE_BARRIER(o) { assert(o.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION); };
			friend inline bool operator< (const TransitionBarrier& lhs, const TransitionBarrier& rhs) {
				if (lhs.Transition.pResource != rhs.Transition.pResource) return lhs.Transition.pResource < rhs.Transition.pResource;
				return lhs.Transition.Subresource < rhs.Transition.Subresource;
			}
			inline void SetStateAfter(D3D12_RESOURCE_STATES state) {
				Transition.StateAfter = state;
			}
			inline bool IsValid() const { return Transition.StateAfter != Transition.StateBefore; }
		};
		struct UAVBarrier : public D3D12_RESOURCE_BARRIER {
			Resource* pResourceRHI = nullptr;
			UAVBarrier(Resource* resource) : D3D12_RESOURCE_BARRIER({}) {
				Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
				pResourceRHI = resource;
				UAV.pResource = resource->GetNativeResource();
			}
			UAVBarrier(const D3D12_RESOURCE_BARRIER& o) : D3D12_RESOURCE_BARRIER(o) { assert(o.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV); };
			friend inline bool operator< (const UAVBarrier& lhs, const UAVBarrier& rhs) {
				return lhs.UAV.pResource < rhs.UAV.pResource;				
			}
		};
		struct AliasingBarrier : public D3D12_RESOURCE_BARRIER {
			Resource* pResourceRHIBefore = nullptr, * pResourceRHIAfter = nullptr;
			AliasingBarrier(Resource* after, Resource* before) : D3D12_RESOURCE_BARRIER({}) {
				Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
				Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
				Aliasing.pResourceAfter = after->GetNativeResource();
				Aliasing.pResourceBefore = before->GetNativeResource();
				pResourceRHIAfter = after;
				pResourceRHIBefore = before;
			}
			AliasingBarrier(const D3D12_RESOURCE_BARRIER& o) : D3D12_RESOURCE_BARRIER(o) { assert(o.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING); };
			friend inline bool operator< (const AliasingBarrier& lhs, const AliasingBarrier& rhs) {
				if (lhs.Aliasing.pResourceAfter != rhs.Aliasing.pResourceAfter) return lhs.Aliasing.pResourceAfter < rhs.Aliasing.pResourceAfter;
				return lhs.Aliasing.pResourceBefore < rhs.Aliasing.pResourceBefore;
			}
		};
		struct {		
			std::vector<D3D12_RESOURCE_BARRIER> BarrierBuffer;
			std::set<TransitionBarrier> TransitionBarriers;
			std::set<UAVBarrier> UAVBarriers;
			std::set<AliasingBarrier> AliasingBarriers;
			inline void QueueTransition(Resource* resource, uint subresource, ResourceState after, uint before = -1) {
				TransitionBarrier barrier(resource, subresource, (uint)after, (uint)before);
				auto it = TransitionBarriers.find(barrier);
				assert(it != TransitionBarriers.end() || before != (uint)- 1); // Need a Before state if the barrier is a initial setup
				if (it != TransitionBarriers.end()) const_cast<TransitionBarrier*>(&*it)->SetStateAfter((D3D12_RESOURCE_STATES)after);
				else TransitionBarriers.insert(barrier);
			}
			inline void QueueUAV(Resource* resource) {
				UAVBarrier barrier(resource);
				UAVBarriers.insert(barrier);
			}
			inline void QueueAliasing(Resource* resourceBefore, Resource* resourceAfter) {
				AliasingBarrier barrier(resourceAfter, resourceBefore);
				AliasingBarriers.insert(barrier);
			}
		} m_Barriers;
		bool m_Closed = false;
		bool m_Reseted = true;
	public:

		CommandList(Device* device, CommandListType type, uint numAllocators = 1);
		~CommandList() = default;
		inline void ResetAllocator(uint allocatorIndex=0) {
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandAllocators[allocatorIndex]->Reset());
			m_Reseted = true;
		};
		inline const CommandListType GetType() { return m_Type; }
		inline void Begin(uint allocatorIndex = 0) {
#if 0			
			if (!m_Reseted) 
				LOG(WARNING) << "Potential Command Allocator leak detected!";
#endif
			CHECK(m_Closed && "Attempting to start an already recording CommandList");
			DCHECK(allocatorIndex < m_CommandAllocators.size());
			CHECK_HR(m_CommandList->Reset(m_CommandAllocators[allocatorIndex].Get(), nullptr));
			m_Closed = false;
			m_Reseted = false;
		};
		inline void Close() { FlushBarriers(); CHECK_HR(m_CommandList->Close()); m_Closed = true; };
		inline bool IsOpen() const { return !m_Closed; }
		inline void QueueTransitionBarrier(Resource* res, ResourceState state, uint subresource) { QueueTransitionBarrier(res, state, &subresource, 1); }
		void QueueTransitionBarrier(Resource* res, ResourceState state, const uint* subresources, uint numSubresources);
		void QueueTransitionBarrier(Resource* res, ResourceState state);
		void QueueUAVBarrier(Resource* res);
		void QueueAliasingBarrier(Resource* before, Resource* after);
		inline void FlushBarriers() { 
			m_Barriers.BarrierBuffer.clear();
			if (m_Barriers.TransitionBarriers.size()) {
				for (auto& transition : m_Barriers.TransitionBarriers)
					if (transition.IsValid()) {
						transition.pResourceRHI->m_States[transition.Transition.Subresource] = (ResourceState)transition.Transition.StateAfter;
						m_Barriers.BarrierBuffer.push_back(transition);
					}
				m_Barriers.TransitionBarriers.clear();
			}
			if (m_Barriers.UAVBarriers.size()) {
				for (auto& uav : m_Barriers.UAVBarriers)
					m_Barriers.BarrierBuffer.push_back(uav);
				m_Barriers.UAVBarriers.clear();
			}
			if (m_Barriers.AliasingBarriers.size()) {
				for (auto& alias : m_Barriers.AliasingBarriers)
					m_Barriers.BarrierBuffer.push_back(alias);
				m_Barriers.AliasingBarriers.clear();
			}
			if (m_Barriers.BarrierBuffer.size())
				m_CommandList->ResourceBarrier(m_Barriers.BarrierBuffer.size(), m_Barriers.BarrierBuffer.data());
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