#pragma once
#include "D3D12Types.hpp"
#define INVALID_HEAP_HANDLE ((uint)-1)
namespace RHI {	
	class DescriptorHeap;
	struct Descriptor {
		friend class DescriptorHeap;
	public:
		Descriptor() {};
		Descriptor(DescriptorHeap* heap) : owner(heap) {};

		auto const& get_heap_handle() { return heap_handle; }
		auto const& get_cpu_handle() { return cpu_handle; }
		auto const& get_gpu_handle() { return gpu_handle; }

		operator D3D12_GPU_DESCRIPTOR_HANDLE() { return gpu_handle; }
		operator D3D12_CPU_DESCRIPTOR_HANDLE() { return cpu_handle; }		
		inline bool operator==(Descriptor lhs) { return lhs.cpu_handle.ptr == cpu_handle.ptr; }
		inline bool is_valid() { return heap_handle != INVALID_HEAP_HANDLE; }
		void release();
	protected:
		DescriptorHeap* owner;

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
		friend struct Descriptor;
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

		inline uint GetCurrentDescriptorCount() { return m_IndexQueue.size(); }
		inline uint GetMaxDescriptorCount() { return m_Config.descriptorCount; };
		void CopyInto(Descriptor descriptorSrc, Descriptor descriptorDst);
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
		Descriptor m_HeadHandle{ this };
		uint m_HeapIncrementSize{};

		numeric_queue<uint> m_IndexQueue;
	};
}