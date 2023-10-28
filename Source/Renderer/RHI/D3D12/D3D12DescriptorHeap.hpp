#pragma once
#include "../Common.hpp"
namespace RHI {
	class Device;
	struct DescriptorHandle {
		handle_type heap_handle;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle{};
		D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
		inline void Increment(size_t offset, UINT heapIncrement) {
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
			CBV_SRV_UAV = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			SAMPLER = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
			RTV = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
			DSV = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
			NUM_TYPES = 4
		};
		struct DescriptorHeapDesc {
			bool shaderVisible;
			UINT descriptorCount;
			HeapType heapType;
		};
		
		DescriptorHeap(Device* device, DescriptorHeapDesc const& cfg);
		~DescriptorHeap() = default;
		
		handle_type Allocate() { return m_HandleQueue.Pop(); }
		inline void Free(handle_type handle) { m_HandleQueue.Return(handle); };

		DescriptorHandle GetDescriptor(handle_type);
		inline bool CheckHandle(handle_type handle) { return handle < m_Config.descriptorCount; };
		inline handle_type GetDescriptorCount() { return m_Config.descriptorCount; };

		inline auto GetNativeHeap() { return m_DescriptorHeap.Get(); }
		inline operator ID3D12DescriptorHeap* () { return m_DescriptorHeap.Get(); }
	protected:
		const DescriptorHeapDesc m_Config;
		Device* const pDevice;

		ComPtr<ID3D12DescriptorHeap> m_DescriptorHeap;
		DescriptorHandle m_HeadHandle{};
		UINT m_HeapIncrementSize{};

		HandleQueue m_HandleQueue;
	};	
}