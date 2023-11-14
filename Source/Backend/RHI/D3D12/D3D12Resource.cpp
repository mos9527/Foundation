#include "D3D12Resource.hpp"
#include "D3D12Device.hpp"
#include "D3D12CommandList.hpp"
namespace RHI {
	void Resource::SetState(CommandList* cmd, ResourceState state, uint subresource) {
		if (m_State != state) {
			auto const transistion = CD3DX12_RESOURCE_BARRIER::Transition(
				m_Resource.Get(),
				(D3D12_RESOURCE_STATES)m_State,
				(D3D12_RESOURCE_STATES)state,
				subresource
			);
			cmd->GetNativeCommandList()->ResourceBarrier(1, &transistion);
			m_State = state;
		}
	}

	void Resource::Map() {
		CHECK(m_Desc.usage != ResourceUsage::Default);
		CHECK_HR(m_Resource->Map(0, nullptr, &pMappedData));
	}

	void Resource::Unmap() {
		CHECK(m_Desc.usage != ResourceUsage::Default);
		m_Resource->Unmap(0, nullptr);
		pMappedData = nullptr;
	}

	void Resource::Update(const void* data, size_t size, size_t offset) {
		CHECK(m_Desc.usage != ResourceUsage::Default);
		CHECK(offset + size <= m_Desc.width);
		if (pMappedData == nullptr) Map();
		memcpy((unsigned char*)pMappedData + offset, data, size);
	}

	Resource::Resource(Device* device, ResourceDesc const& desc) : DeviceChild(device), m_Desc(desc) {
		m_State = desc.initialState;
		D3D12MA::ALLOCATION_DESC allocationDesc{};				
		allocationDesc.HeapType = ResourceUsageToD3DHeapType(desc.usage);


		const D3D12_RESOURCE_DESC resourceDesc = desc;
		D3D12_CLEAR_VALUE clearValue = desc.clearValue;
		auto allocator = device->GetAllocator();
		clearValue.Format = ResourceFormatToD3DFormat(desc.format);
		CHECK_HR(allocator->CreateResource(
			&allocationDesc,
			&resourceDesc,
			(D3D12_RESOURCE_STATES)m_State,
			desc.clearValue.clear ? &clearValue : nullptr,
			&m_Allocation,
			IID_PPV_ARGS(&m_Resource)
		));
	}

	Resource::Resource(Device* device, ResourceDesc const& desc, const void* data, size_t size, size_t offset) :
		Resource(device, desc) {		
		Update(data, size, offset);
	}

}