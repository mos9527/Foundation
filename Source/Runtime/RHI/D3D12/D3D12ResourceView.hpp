#include "D3D12Device.hpp"
#include "D3D12Resource.hpp"

namespace RHI {
	template<typename Desc> struct ResourceView {
		const Desc viewDesc;
		const Resource* resource;		
		Descriptor descriptor;

		inline Desc const& GetDesc() { return viewDesc; }
		ResourceView(Resource* resource, Desc const& viewDesc) : viewDesc(viewDesc), resource(resource) {};
	};
	struct ShaderResourceView : public ResourceView<ShaderResourceViewDesc> {
		ShaderResourceView(Resource* resource, ShaderResourceViewDesc const& desc) : ResourceView(resource, desc) {
			auto device = resource->GetParent();
			descriptor = device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor(); // xxx Online only? enough for now but...
			const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = desc;
			device->GetNativeDevice()->CreateShaderResourceView(*resource, &srvDesc, descriptor.get_cpu_handle());
		}
	};
	struct UnorderedAccessView : public ResourceView<UnorderedAccessViewDesc> {
		UnorderedAccessView(Resource* resource, Resource* counterResource, UnorderedAccessViewDesc const& desc) : ResourceView(resource, desc) {
			auto device = resource->GetParent();
			descriptor = device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = desc;
			device->GetNativeDevice()->CreateUnorderedAccessView(*resource, counterResource ? *counterResource : NULL, &uavDesc, descriptor.get_cpu_handle());
		}
		UnorderedAccessView(Resource* resource, UnorderedAccessViewDesc const& desc) : UnorderedAccessView(resource, nullptr, desc) {};
	};
	struct RenderTargetView : public ResourceView<RenderTargetViewDesc> {
		RenderTargetView(Resource* resource, RenderTargetViewDesc const& desc) : ResourceView(resource, desc) {
			auto device = resource->GetParent();
			descriptor = device->GetDescriptorHeap<DescriptorHeapType::RTV>()->AllocateDescriptor();
			const D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = desc;
			device->GetNativeDevice()->CreateRenderTargetView(*resource, &rtvDesc, descriptor.get_cpu_handle());
		}
		RenderTargetView(Resource* resource) : ResourceView(resource, RenderTargetViewDesc{}) {
			auto device = resource->GetParent();
			descriptor = device->GetDescriptorHeap<DescriptorHeapType::RTV>()->AllocateDescriptor();			
			device->GetNativeDevice()->CreateRenderTargetView(*resource, NULL, descriptor.get_cpu_handle());
		}
	};
	struct DepthStencilView : public ResourceView<DepthStencilViewDesc> {
		DepthStencilView(Resource* resource, DepthStencilViewDesc const& desc) : ResourceView(resource, desc) {
			auto device = resource->GetParent();
			descriptor = device->GetDescriptorHeap<DescriptorHeapType::DSV>()->AllocateDescriptor();
			const D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = desc;
			device->GetNativeDevice()->CreateDepthStencilView(*resource, &dsvDesc, descriptor.get_cpu_handle());
		}
	};
}