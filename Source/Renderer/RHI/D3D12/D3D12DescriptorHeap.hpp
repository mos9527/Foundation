#pragma once
#include "../Common.hpp"
namespace RHI {
	class Device;
	class DescriptorHeap;
	typedef uint heap_handle_type;
	constexpr heap_handle_type INVALID_HEAP_HANDLE = -1;
	struct Descriptor {
		friend class DescriptorHeap;
	public:
		auto const& get_heap_handle() { return heap_handle; }
		auto const& get_cpu_handle() { return cpu_handle; }
		auto const& get_gpu_handle() { return gpu_handle; }

		operator D3D12_GPU_DESCRIPTOR_HANDLE() { return gpu_handle; }
		operator D3D12_CPU_DESCRIPTOR_HANDLE() { return cpu_handle; }		
		inline bool operator==(Descriptor lhs) { return lhs.cpu_handle.ptr == cpu_handle.ptr; }
		inline bool IsValid() { return heap_handle != INVALID_HEAP_HANDLE; }

		Descriptor(DescriptorHeap& _heap_ref) : heap_ref(_heap_ref) {};
		Descriptor(Descriptor& other) : heap_ref(other.heap_ref) {
			heap_handle = other.heap_handle;
			cpu_handle = other.cpu_handle;
			gpu_handle = other.gpu_handle;
		}
		Descriptor(Descriptor&& other) = delete;
		Descriptor() = delete; // not default constructable
		~Descriptor();

	private:
		DescriptorHeap& heap_ref;
		heap_handle_type heap_handle{ INVALID_HEAP_HANDLE };
		D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle{};
		D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
		inline void Increment(size_t offset, uint heapIncrement) {
			heap_handle += offset;
			cpu_handle.ptr += offset * heapIncrement;
			if (gpu_handle.ptr) gpu_handle.ptr += offset * heapIncrement;
		}
	};
	class DescriptorHeap : public RHIObject {
		friend struct Descriptor;
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
		
		std::shared_ptr<Descriptor> GetDescriptor();
		inline heap_handle_type GetCurrentDescriptorCount() { return m_IndexQueue.size(); }
		inline heap_handle_type GetMaxDescriptorCount() { return m_Config.descriptorCount; };

		inline auto GetNativeHeap() { return m_DescriptorHeap.Get(); }
		inline operator ID3D12DescriptorHeap* () { return m_DescriptorHeap.Get(); }
		
		using RHIObject::GetName;
		inline void SetName(name_t name) { 
			m_Name = name; 
			m_DescriptorHeap->SetName((const wchar_t*)name.c_str()); 
		}
	protected:
		inline void Free(heap_handle_type handle) { m_IndexQueue.push(handle); };

		using RHIObject::m_Name;
		const DescriptorHeapDesc m_Config;

		ComPtr<ID3D12DescriptorHeap> m_DescriptorHeap;
		Descriptor m_HeadHandle{ *this };
		uint m_HeapIncrementSize{};

		numeric_queue<heap_handle_type> m_IndexQueue;
	};	
}