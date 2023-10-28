#include "D3D12Buffer.hpp"
#include "D3D12Device.hpp"
#include "D3D12CommandList.hpp"
namespace RHI {
	Buffer::Buffer(Device* device, BufferDesc const& desc) : m_Desc(desc) {
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

	void Buffer::SetState(CommandList* cmdList, ResourceState state, UINT subresource) {
		auto const transistion = CD3DX12_RESOURCE_BARRIER::Transition(
			m_Resource.Get(),
			(D3D12_RESOURCE_STATES)m_State,
			(D3D12_RESOURCE_STATES)state,
			subresource
		);
		cmdList->GetNativeCommandList()->ResourceBarrier(1, &transistion);
		m_State = state;
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
		if (pMappedData == nullptr) Map();
		memcpy((char*)pMappedData + offset, data, size);
	}

	Buffer::Buffer(Device* device, BufferDesc const& desc, const void* data, size_t size, size_t offset) :
		Buffer(device, desc) {		
		Update(data, size, offset);
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
}