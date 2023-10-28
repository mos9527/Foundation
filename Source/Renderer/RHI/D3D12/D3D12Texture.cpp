#include "D3D12Texture.hpp"
#include "D3D12Device.hpp"
namespace RHI {	
	Texture::Texture(Device* device, TextureDesc const& desc) : Buffer(desc) {
		m_State = desc.initialState;
		D3D12MA::ALLOCATION_DESC allocationDesc{};		
		switch (desc.usage) {
		case ResourceUsage::Upload:
			allocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;			
			break;
		case ResourceUsage::Readback:
			allocationDesc.HeapType = D3D12_HEAP_TYPE_READBACK;			
			break;
		default:
		case ResourceUsage::Default:
			allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;			
			break;
		};
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
	Texture::Texture(Device* device, TextureDesc const& desc, CommandList* cmdList, SubresourceData* data, UINT count):
		Texture(device,desc) {
		DCHECK_ENUM_FLAG(m_State & ResourceState::CopyDest);
		const D3D12_RESOURCE_DESC resourceDesc = desc;
		size_t intermediateSize;
		device->GetNativeDevice()->GetCopyableFootprints(
			&resourceDesc, 0, count, 0, NULL, NULL, NULL, &intermediateSize
		);
		auto bufDesc = Buffer::BufferDesc::GetGenericBufferDesc(intermediateSize);
		std::unique_ptr<Buffer> intermediate = std::make_unique<Buffer>(device, bufDesc);
		std::vector<D3D12_SUBRESOURCE_DATA> arrData;
		for (UINT i = 0; i < count; i++) arrData.push_back(data[i]);
		UpdateSubresources(
			cmdList->GetNativeCommandList(),
			*this,
			*intermediate,
			0, 0, count, arrData.data()
		);
	}
}