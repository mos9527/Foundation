#include "D3D12Buffer.hpp"
#include "D3D12Device.hpp"
#include "D3D12CommandList.hpp"
namespace RHI {
	void Buffer::SetState(CommandList* cmdList, ResourceState state, uint subresource) {
		if (m_State != state) {
			auto const transistion = CD3DX12_RESOURCE_BARRIER::Transition(
				m_Resource.Get(),
				(D3D12_RESOURCE_STATES)m_State,
				(D3D12_RESOURCE_STATES)state,
				subresource
			);
			cmdList->GetNativeCommandList()->ResourceBarrier(1, &transistion);
			m_State = state;
		}
	}

	void Buffer::Map() {
		DCHECK(m_Desc.usage != ResourceUsage::Default);
		CHECK_HR(m_Resource->Map(0, nullptr, &pMappedData));
	}

	void Buffer::Unmap() {
		DCHECK(m_Desc.usage != ResourceUsage::Default);
		m_Resource->Unmap(0, nullptr);
		pMappedData = nullptr;
	}

	void Buffer::Update(const void* data, size_t size, size_t offset) {
		CHECK(offset + size <= m_Desc.width);
		if (pMappedData == nullptr) Map();
		memcpy((unsigned char*)pMappedData + offset, data, size);
	}

	void Buffer::QueueCopy(CommandList* cmdList, Buffer* srcBuffer, size_t srcOffset, size_t dstOffset, size_t size) {
		DCHECK(m_Resource.Get());
		DCHECK(srcBuffer->m_Resource.Get());
		DCHECK_ENUM_FLAG(m_State & ResourceState::CopyDest);
		DCHECK_ENUM_FLAG(srcBuffer->GetState() & ResourceState::CopySource);		
		cmdList->GetNativeCommandList()->CopyBufferRegion(
			m_Resource.Get(), dstOffset, srcBuffer->GetNativeBuffer(), srcOffset, size
		);
	}

	Buffer::Buffer(Device* device, BufferDesc const& desc) : DeviceChild(device), m_Desc(desc) {
		m_State = desc.initialState;
		D3D12MA::ALLOCATION_DESC allocationDesc{};		
		allocationDesc.CustomPool = device->GetAllocatorPool(desc.poolType);
		allocationDesc.HeapType = ResourceUsageToD3DHeapType(desc.usage);

		const D3D12_RESOURCE_DESC resourceDesc = desc;
		auto allocator = device->GetAllocator();
		CHECK_HR(allocator->CreateResource(
			&allocationDesc,
			&resourceDesc,
			(D3D12_RESOURCE_STATES)m_State,
			nullptr,
			&m_Allocation,
			IID_PPV_ARGS(&m_Resource)
		));
	}

	Buffer::Buffer(Device* device, BufferDesc const& desc, const void* data, size_t size, size_t offset) :
		Buffer(device, desc) {		
		Update(data, size, offset);
	}

}