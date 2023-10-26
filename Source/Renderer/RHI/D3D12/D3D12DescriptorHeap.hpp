#pragma once
#include "../RHICommon.hpp"
#include "D3D12Descriptor.hpp"
namespace RHI {
	class Device;
	class DescriptorHeap {
	public:
		typedef size_t handle_t;
		const handle_t invalid_handle = -1;

		enum HeapType {
			CBV_SRV_UAV = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			SAMPLER = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
			RTV = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
			DSV = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
			NUM_TYPES = 4
		};
		struct DescriptorHeapConfig {
			bool ShaderVisible;
			UINT DescriptorCount;
			HeapType Type;
		};
		DescriptorHeap(Device* device, DescriptorHeapConfig const& cfg);
		~DescriptorHeap() = default;
		Descriptor GetByHandle(handle_type index);
		inline UINT GetHeapSize() { return m_Config.DescriptorCount; };
		inline bool IsValid(Descriptor hdl) { return hdl == GetByHandle(hdl.handle); }
		inline operator ID3D12DescriptorHeap* () { return m_DescriptorHeap.Get(); }
	protected:
		const DescriptorHeapConfig m_Config;
		ComPtr<ID3D12DescriptorHeap> m_DescriptorHeap;
		Descriptor m_HeadHandle{};
		UINT m_HeapIncrementSize{};
	};

	class DescriptorHeapAllocator : public DescriptorHeap {
	private:
		std::vector<bool> m_DescriptorStates;
	public:
		DescriptorHeapAllocator(Device* device, DescriptorHeapConfig const& cfg);
		~DescriptorHeapAllocator() = default;
		using DescriptorHeap::operator ID3D12DescriptorHeap*;
		Descriptor Allocate();
		void Free(handle_t);
	};
}