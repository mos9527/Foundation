#pragma once
#include "../RHICommon.hpp"
namespace RHI {
	class DescriptorHeap;
	struct Descriptor {
		struct Offset {
			const handle_type offset;
			const UINT heapIncrement;
			Offset(UINT HeapIncrement) : offset(1), heapIncrement(HeapIncrement) {};
			Offset(handle_type OffsetIndex, UINT HeapIncrement) : offset(OffsetIndex), heapIncrement(HeapIncrement) {};
		};
		handle_type handle = invalid_handle;

		D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle{};
		D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
		void Increment(Offset offset) {
			cpu_handle.ptr += (size_t)offset.offset * offset.heapIncrement;
			if (gpu_handle.ptr) gpu_handle.ptr += (size_t)offset.offset * offset.heapIncrement;
			handle += offset.offset;
		}
		Descriptor operator+ (Offset offset) {
			Descriptor hdl = *this;
			hdl.Increment(offset);
			return hdl;
		}
		operator D3D12_GPU_DESCRIPTOR_HANDLE() { return gpu_handle; }
		operator D3D12_CPU_DESCRIPTOR_HANDLE() { return cpu_handle; }
		inline bool IsValid() { return handle != -1 && cpu_handle.ptr != 0; }
		inline bool operator==(Descriptor lhs) {
			return handle == lhs.handle && lhs.cpu_handle.ptr == cpu_handle.ptr;
		}
	};
}