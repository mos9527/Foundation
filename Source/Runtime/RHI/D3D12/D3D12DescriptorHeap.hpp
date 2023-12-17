#pragma once
#include "D3D12Types.hpp"
#include "D3D12Fence.hpp"
#define INVALID_HEAP_HANDLE ((uint)-1)
namespace RHI {	
	class DescriptorHeap;
	struct Descriptor {
		friend class DescriptorHeap;
	public:
		UINT const& get_heap_handle() const { return heap_handle; }
		D3D12_CPU_DESCRIPTOR_HANDLE const& get_cpu_handle()  const { return cpu_handle; }
		D3D12_GPU_DESCRIPTOR_HANDLE const& get_gpu_handle()  const { return gpu_handle; }

		operator D3D12_GPU_DESCRIPTOR_HANDLE() const { return gpu_handle; }
		operator D3D12_CPU_DESCRIPTOR_HANDLE() const { return cpu_handle; }		
		inline bool operator==(Descriptor lhs) const { return lhs.cpu_handle.ptr == cpu_handle.ptr; }
		inline bool is_valid() const { return heap_handle != INVALID_HEAP_HANDLE; }		
		inline void invalidate() { heap_handle = INVALID_HEAP_HANDLE; }
	protected:		
		uint heap_handle{ INVALID_HEAP_HANDLE };
		D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle{};
		D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
		inline void Increment(size_t offset, uint heapIncrement) {
			heap_handle += offset;
			cpu_handle.ptr += offset * heapIncrement;
			if (gpu_handle.ptr) gpu_handle.ptr += offset * heapIncrement;
		}
	};
	class DescriptorHeap : public DeviceChild {
		friend Descriptor;
		std::mutex alloc_mutex;
	public:
		struct DescriptorHeapDesc {
			bool shaderVisible;
			uint descriptorCount;
			DescriptorHeapType heapType;
		};
		
		DescriptorHeap(Device* device, DescriptorHeapDesc const& cfg);
		~DescriptorHeap() = default;
		
		Descriptor GetDescriptor(uint handle);
		Descriptor AllocateDescriptor();
		void FreeDescriptor(uint handle);
		void FreeDescriptor(Descriptor desc);

		inline uint GetCurrentDescriptorCount() { return m_FreeList.size(); }
		inline uint GetMaxDescriptorCount() { return m_Config.descriptorCount; };
		inline auto GetNativeHeap() { return m_DescriptorHeap.Get(); }
		inline operator ID3D12DescriptorHeap* () { return m_DescriptorHeap.Get(); }
		
		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) { 
			m_Name = name; 
			m_DescriptorHeap->SetName(name); 
		}
	protected:
		name_t m_Name;
		const DescriptorHeapDesc m_Config;

		ComPtr<ID3D12DescriptorHeap> m_DescriptorHeap;
		Descriptor m_HeadHandle{};
		uint m_HeapIncrementSize{};

		free_list<uint> m_FreeList;
	};

	class OnlineDescriptorHeap;
	struct TransientDescriptorPage;
	struct TransientDescriptorPage {
		uint start, end, allocated;
		inline bool IsFull() const { return start + allocated >= end; }
		inline uint AllocateDescriptor() {
			CHECK(!IsFull());
			return start + (allocated++);
		}
		inline void Reset() { allocated = 0; }
	};
	class OnlineDescriptorHeap : public DeviceChild {
		friend Descriptor;
		name_t m_Name;

		DescriptorHeap m_Heap;

		std::vector<TransientDescriptorPage> m_Pages;
		free_list<uint> m_FreePages;
		std::vector<uint> m_FilledPages;
		std::deque<std::pair<SyncFence, uint>> m_PagesToFree;

		free_list<uint> m_PersistentFreeDescriptors;

		std::vector<uint> m_CurrentPage;

		void TryCleanPages();
		uint AllocatePage();

		std::mutex alloc_mutex;
	public:
		Descriptor GetDescriptor(uint handle) { return m_Heap.GetDescriptor(handle); }
		OnlineDescriptorHeap(Device* device, DescriptorHeap::DescriptorHeapDesc const& cfg, uint pageSize = 128, uint persistentCount = 128);

		Descriptor AllocatePersistentDescriptor();
		void FreePersistentDescriptor(Descriptor& descriptor);

		Descriptor AllocateTransitentDescriptor(CommandListType ctxType);

		void SetCleanupSyncPoint(SyncFence const& sync);

		inline auto GetNativeHeap() { return m_Heap.GetNativeHeap(); }
		inline operator ID3D12DescriptorHeap* () { return GetNativeHeap(); }

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;
			m_Heap.SetName(name);
		}
	};
}