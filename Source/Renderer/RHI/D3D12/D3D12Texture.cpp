#include "D3D12Texture.hpp"
#include "D3D12Device.hpp"
namespace RHI {	
	Texture::Texture(Device* device, TextureDesc const& desc) : Buffer(desc) {
		m_State = desc.initialState;
		D3D12MA::ALLOCATION_DESC allocationDesc{};		
		allocationDesc.CustomPool = device->GetAllocatorPool(desc.poolType);
		allocationDesc.HeapType = ResourceUsageToD3DHeapType(desc.usage);

		const D3D12_RESOURCE_DESC resourceDesc = desc;
		D3D12_CLEAR_VALUE clearValue = desc.clearValue;		
		auto allocator = device->GetAllocator();		
		CHECK_HR(allocator->CreateResource(
			&allocationDesc,
			&resourceDesc,
			(D3D12_RESOURCE_STATES)m_State,
			desc.clearValue.clear ? &clearValue : nullptr,
			&m_Allocation,
			IID_PPV_ARGS(&m_Resource)
		));
	}
	Texture::Texture(Device* device, TextureDesc const& desc, CommandList* cmdList, SubresourceData* data, uint count):
		Texture(device,desc) {
		DCHECK_ENUM_FLAG(m_State & ResourceState::CopyDest);
		const D3D12_RESOURCE_DESC resourceDesc = desc;
		size_t intermediateSize;
		device->GetNativeDevice()->GetCopyableFootprints(
			&resourceDesc, 0, count, 0, NULL, NULL, NULL, &intermediateSize
		);
		auto bufDesc = Buffer::BufferDesc::GetGenericBufferDesc(intermediateSize);
		bufDesc.poolType = ResourcePoolType::Intermediate;
		auto intermediate = device->AllocateIntermediateBuffer(bufDesc);		
		std::vector<D3D12_SUBRESOURCE_DATA> arrData;
		for (uint i = 0; i < count; i++) arrData.push_back(data[i]);
		UpdateSubresources(
			cmdList->GetNativeCommandList(),
			*this,
			*intermediate,
			0, 0, count, arrData.data()
		);
	}
}