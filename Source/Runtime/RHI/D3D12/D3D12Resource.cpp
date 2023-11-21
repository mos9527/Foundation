#include "D3D12Resource.hpp"
#include "D3D12Device.hpp"
#include "D3D12CommandList.hpp"
namespace RHI {
	void Resource::SetBarrier(CommandList* cmd, ResourceState state, uint subresource) {
		if (m_States[subresource] != state) {
			auto const transistion = CD3DX12_RESOURCE_BARRIER::Transition(
				m_Resource.Get(),
				(D3D12_RESOURCE_STATES)m_States[subresource],
				(D3D12_RESOURCE_STATES)state,
				subresource
			);
			cmd->GetNativeCommandList()->ResourceBarrier(1, &transistion);
			m_States[subresource] = state;
		}
	}
	void Resource::SetBarrier(CommandList* cmd, ResourceState state, std::vector<UINT>&& subresources) {
		std::vector<CD3DX12_RESOURCE_BARRIER> transitions;
		for (UINT subresource : subresources) {
			if (m_States[subresource] != state) {
				transitions.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
					m_Resource.Get(),
					(D3D12_RESOURCE_STATES)m_States[subresource],
					(D3D12_RESOURCE_STATES)state,
					subresource
				));
				m_States[subresource] = state;
			}
		}
		if (transitions.size())
			cmd->GetNativeCommandList()->ResourceBarrier(transitions.size(), transitions.data());
	}
	void Resource::SetBarrier(CommandList* cmd, ResourceState state) {
		std::vector<UINT> subresources(m_Desc.numSubresources());
		std::iota(subresources.begin(), subresources.end(), 0);
		SetBarrier(cmd, state, std::move(subresources));
	}
	void Resource::Map(size_t read_begin, size_t read_end) {
		CHECK(m_Desc.heapType != ResourceHeapType::Default);
		D3D12_RANGE read_range{ .Begin = read_begin, .End = read_end };
		CHECK_HR(m_Resource->Map(0, &read_range, &pMappedData));
	}

	void Resource::Unmap() {
		CHECK(m_Desc.heapType != ResourceHeapType::Default);
		m_Resource->Unmap(0, nullptr);
		pMappedData = nullptr;
	}

	void Resource::Update(const void* data, size_t size, size_t offset) {
		CHECK(m_Desc.heapType != ResourceHeapType::Default);
		CHECK(offset + size <= m_Desc.width);
		if (pMappedData == nullptr) Map();
		memcpy((unsigned char*)pMappedData + offset, data, size);
	}

	Resource::Resource(Device* device, ResourceDesc const& desc) : DeviceChild(device), m_Desc(desc) {
		m_States.resize(desc.numSubresources());
		for (auto& state : m_States) state = desc.initialState;
		D3D12MA::ALLOCATION_DESC allocationDesc{};
		allocationDesc.HeapType = ResourceHeapTypeToD3DHeapType(desc.heapType);

		const D3D12_RESOURCE_DESC resourceDesc = desc;
		CHECK(!desc.clearValue.has_value() || (+(desc.flags & ResourceFlags::RenderTarget) || +(desc.flags & ResourceFlags::DepthStencil)) && "Clear value only applicable to RenderTargets and DepthStencils");
		D3D12_CLEAR_VALUE clearValue = desc.clearValue.has_value() ? desc.clearValue.value().ToD3D12ClearValue(desc.format) : D3D12_CLEAR_VALUE();
		auto allocator = device->GetAllocator();
		CHECK_HR(allocator->CreateResource(
			&allocationDesc,
			&resourceDesc,
			(D3D12_RESOURCE_STATES)desc.initialState,
			desc.clearValue.has_value() ? &clearValue : NULL,
			&m_Allocation,
			IID_PPV_ARGS(&m_Resource)
		));
		if (desc.resourceName)
			SetName(desc.resourceName);
	}
	static Resource::ResourceDesc FromD3D12Resource(ID3D12Resource* resource) {
		D3D12_RESOURCE_DESC desc = resource->GetDesc();
		return Resource::ResourceDesc{
			.clearValue = {},
			.heapType = ResourceHeapType::Unknown,
			.format = (ResourceFormat)desc.Format,
			.dimension = (ResourceDimension)desc.Dimension,
			.alignment = desc.Alignment,
			.width = desc.Width,
			.height = desc.Height,
			.mipLevels = desc.MipLevels,
			.arraySize = desc.DepthOrArraySize,
			.sampleCount = desc.SampleDesc.Count,
			.sampleQuality = desc.SampleDesc.Quality,
			.layout = (TextureLayout)desc.Layout
		};
	}
	Resource::Resource(Device* device, ComPtr<ID3D12Resource>&& texture , name_t name) : DeviceChild(device), m_Resource(texture) , m_Desc(FromD3D12Resource(texture.Get())) , m_Name(L"<imported>")
	{
		if (name) 
			SetName(name);
		m_States.resize(m_Desc.numSubresources());
	}

}