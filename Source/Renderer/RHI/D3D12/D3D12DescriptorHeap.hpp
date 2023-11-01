#pragma once
#include "../Common.hpp"
namespace RHI {
	class Device;
	struct DescriptorHandle {
		handle heap_handle;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle{};
		D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
		inline void Increment(size_t offset, uint heapIncrement) {
			heap_handle += offset;
			cpu_handle.ptr += offset * heapIncrement;
			if (gpu_handle.ptr) gpu_handle.ptr += offset * heapIncrement;			
		}
		operator D3D12_GPU_DESCRIPTOR_HANDLE() { return gpu_handle; }
		operator D3D12_CPU_DESCRIPTOR_HANDLE() { return cpu_handle; }		
		inline bool operator==(DescriptorHandle lhs) { return lhs.cpu_handle.ptr == cpu_handle.ptr; }
	};
	class DescriptorHeap {
	public:
		enum HeapType {
			CBV_SRV_UAV = 0,
			SAMPLER = 1,
			RTV = 2,
			DSV = 3,
			NUM_TYPES = 4			
		};
		struct DescriptorHeapDesc {
			bool shaderVisible;
			uint descriptorCount;
			HeapType heapType;
		};
		
		DescriptorHeap(Device* device, DescriptorHeapDesc const& cfg);
		~DescriptorHeap() = default;
		inline void SetName(name_t name) { m_Name = name; m_DescriptorHeap->SetName((const wchar_t*)name.c_str()); }
		
		DescriptorHandle Allocate() { return GetDescriptor(m_HandleQueue.pop()); }
		inline void Free(DescriptorHandle handle) { m_HandleQueue.push(handle.heap_handle); };

		DescriptorHandle GetDescriptor(handle heap_handle);
		inline bool CheckHandle(handle heap_handle) { return heap_handle < m_Config.descriptorCount; };
		inline handle GetDescriptorCount() { return m_Config.descriptorCount; };

		inline auto GetNativeHeap() { return m_DescriptorHeap.Get(); }
		inline operator ID3D12DescriptorHeap* () { return m_DescriptorHeap.Get(); }
	protected:
		name_t m_Name;
		const DescriptorHeapDesc m_Config;
		Device* const pDevice;

		ComPtr<ID3D12DescriptorHeap> m_DescriptorHeap;
		DescriptorHandle m_HeadHandle{};
		uint m_HeapIncrementSize{};

		handle_queue m_HandleQueue;
	};	
}