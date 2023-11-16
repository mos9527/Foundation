#include "D3D12Device.hpp"
#include "D3D12Resource.hpp"

namespace RHI {
	template<typename Desc> struct ResourceView {
		const Desc viewDesc;
		const Resource* resource;
		Descriptor descriptor;

		inline Desc const& GetDesc() { return viewDesc; }
		ResourceView(Resource* resource, Desc const& viewDesc) : descriptor(descriptor), viewDesc(viewDesc), resource(resource) {};
	};
	struct ShaderResourceView : public ResourceView<ShaderResourceViewDesc> {
		ShaderResourceView(Resource* resource, ShaderResourceViewDesc const& desc) : ResourceView(resource, desc) {
			auto device = resource->GetParent();
			descriptor = device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor(); // xxx Online only? enough for now but...
			device->GetNativeDevice()->CreateShaderResourceView(*resource, &desc, descriptor.get_cpu_handle());
		}
	};
	struct UnorderedAccessView : public ResourceView<UnorderedAccessViewDesc> {
		UnorderedAccessView(Resource* resource, UnorderedAccessViewDesc const& desc) : ResourceView(resource, desc) {
			auto device = resource->GetParent();
			descriptor = device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			device->GetNativeDevice()->CreateUnorderedAccessView(*resource, NULL, &desc, descriptor.get_cpu_handle()); // xxx Counter Resource?
		}
	};
	struct RenderTargetView : public ResourceView<RenderTargetViewDesc> {
		RenderTargetView(Resource* resource, RenderTargetViewDesc const& desc) : ResourceView(resource, desc) {
			auto device = resource->GetParent();
			descriptor = device->GetDescriptorHeap<DescriptorHeapType::RTV>()->AllocateDescriptor();
			device->GetNativeDevice()->CreateRenderTargetView(*resource, &desc, descriptor.get_cpu_handle());
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
			device->GetNativeDevice()->CreateDepthStencilView(*resource, &desc, descriptor.get_cpu_handle());
		}
	};
}