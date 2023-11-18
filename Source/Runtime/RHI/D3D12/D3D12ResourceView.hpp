#include "D3D12Device.hpp"
#include "D3D12Resource.hpp"

namespace RHI {
	template<typename Desc> struct ResourceView : public DeviceChild {
		const Desc viewDesc;
		Resource* const resource;
		Descriptor descriptor;

		inline Desc const& GetDesc() { return viewDesc; }		
		ResourceView(Resource* resource, Desc const& viewDesc) : DeviceChild(resource->GetParent()), viewDesc(viewDesc), resource(resource) {};
		ResourceView(ResourceView&& other) noexcept: ResourceView(other.resource, other.viewDesc) {
			descriptor = other.descriptor;
			other.descriptor.invalidate();
		}
	};
	struct ShaderResourceView : public ResourceView<ShaderResourceViewDesc> {
		ShaderResourceView(Resource* resource, ShaderResourceViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor(); // xxx Online only? enough for now but...
			const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = desc;
			m_Device->GetNativeDevice()->CreateShaderResourceView(*resource, &srvDesc, descriptor.get_cpu_handle());
		}
		~ShaderResourceView() {
			m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreeDescriptor(descriptor);
			descriptor.invalidate();
		}
		ShaderResourceView(ShaderResourceView&& other) noexcept: ResourceView(std::move(other)) {};
	};
	struct UnorderedAccessView : public ResourceView<UnorderedAccessViewDesc> {
		UnorderedAccessView(Resource* resource, Resource* counterResource, UnorderedAccessViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = desc;
			m_Device->GetNativeDevice()->CreateUnorderedAccessView(*resource, counterResource ? *counterResource : NULL, &uavDesc, descriptor.get_cpu_handle());
		}
		UnorderedAccessView(Resource* resource, UnorderedAccessViewDesc const& desc) : UnorderedAccessView(resource, nullptr, desc) {};
		~UnorderedAccessView() {
			m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreeDescriptor(descriptor);
			descriptor.invalidate();
		}
		UnorderedAccessView(UnorderedAccessView&& other) noexcept: ResourceView(std::move(other)) {};
	};
	struct RenderTargetView : public ResourceView<RenderTargetViewDesc> {
		RenderTargetView(Resource* resource, RenderTargetViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::RTV>()->AllocateDescriptor();
			const D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = desc;
			m_Device->GetNativeDevice()->CreateRenderTargetView(*resource, &rtvDesc, descriptor.get_cpu_handle());
		}
		RenderTargetView(Resource* resource) : ResourceView(resource, RenderTargetViewDesc{}) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::RTV>()->AllocateDescriptor();			
			m_Device->GetNativeDevice()->CreateRenderTargetView(*resource, NULL, descriptor.get_cpu_handle());
		}
		~RenderTargetView() {
			m_Device->GetDescriptorHeap<DescriptorHeapType::RTV>()->FreeDescriptor(descriptor);
			descriptor.invalidate();
		}
		RenderTargetView(RenderTargetView&& other) noexcept: ResourceView(std::move(other)) {};
	};
	struct DepthStencilView : public ResourceView<DepthStencilViewDesc> {
		DepthStencilView(Resource* resource, DepthStencilViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::DSV>()->AllocateDescriptor();
			const D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = desc;
			m_Device->GetNativeDevice()->CreateDepthStencilView(*resource, &dsvDesc, descriptor.get_cpu_handle());
		}
		~DepthStencilView() {
			m_Device->GetDescriptorHeap<DescriptorHeapType::DSV>()->FreeDescriptor(descriptor);
			descriptor.invalidate();
		}
		DepthStencilView(DepthStencilView&& other) noexcept: ResourceView(std::move(other)) {};
	};
	struct Sampler : public ResourceView<SamplerDesc> {

	};
}