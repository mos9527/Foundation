#include "D3D12Device.hpp"
#include "D3D12Resource.hpp"

namespace RHI {
	template<typename Desc> struct ResourceView : public DeviceChild {
	protected:
		Descriptor descriptor;
	public:
		const Desc viewDesc;
		Resource* const resource;

		inline Desc const& GetDesc() { return viewDesc; }		
		ResourceView(Resource* resource, Desc const& viewDesc) : DeviceChild(resource->GetParent()), viewDesc(viewDesc), resource(resource) {};
	};
	struct ShaderResourceView : public ResourceView<ShaderResourceViewDesc> {
	public:
		inline Descriptor const& get_storage_descriptor() { return descriptor;  }
		inline const Descriptor allocate_online_descriptor() {
			auto online_descriptor = m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			m_Device->GetNativeDevice()->CopyDescriptorsSimple(1, online_descriptor.get_cpu_handle(), descriptor.get_cpu_handle(), DescriptorHeapTypeToD3DType(DescriptorHeapType::CBV_SRV_UAV));
			return online_descriptor;
		}
		ShaderResourceView(Resource* resource, ShaderResourceViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = desc;
			m_Device->GetNativeDevice()->CreateShaderResourceView(*resource, &srvDesc, descriptor.get_cpu_handle());
		}
		~ShaderResourceView() {
			m_Device->GetDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreeDescriptor(descriptor);
			descriptor.invalidate();
		}
		ShaderResourceView(ShaderResourceView&& other) noexcept: ResourceView(std::move(other)) {};
	};
	struct UnorderedAccessView : public ResourceView<UnorderedAccessViewDesc> {
	public:
		inline Descriptor const& get_storage_descriptor() { return descriptor; }
		inline const Descriptor allocate_online_descriptor() {
			auto online_descriptor = m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			m_Device->GetNativeDevice()->CopyDescriptorsSimple(1, online_descriptor.get_cpu_handle(), descriptor.get_cpu_handle(), DescriptorHeapTypeToD3DType(DescriptorHeapType::CBV_SRV_UAV));
			return online_descriptor;
		}
		UnorderedAccessView(Resource* resource, Resource* counterResource, UnorderedAccessViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = desc;
			m_Device->GetNativeDevice()->CreateUnorderedAccessView(*resource, counterResource ? *counterResource : NULL, &uavDesc,descriptor.get_cpu_handle());
		}
		UnorderedAccessView(Resource* resource, UnorderedAccessViewDesc const& desc) : UnorderedAccessView(resource, nullptr, desc) {};
		~UnorderedAccessView() {
			m_Device->GetDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreeDescriptor(descriptor);
			descriptor.invalidate();
		}
		UnorderedAccessView(UnorderedAccessView&& other) noexcept: ResourceView(std::move(other)) {};
	};
	struct RenderTargetView : public ResourceView<RenderTargetViewDesc> {
		inline Descriptor const& get_descriptor() { return descriptor; }
		RenderTargetView(Resource* resource, RenderTargetViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::RTV>()->AllocateDescriptor();
			const D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = desc;
			m_Device->GetNativeDevice()->CreateRenderTargetView(*resource, &rtvDesc,descriptor.get_cpu_handle());
		}
		RenderTargetView(Resource* resource) : ResourceView(resource, RenderTargetViewDesc{}) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::RTV>()->AllocateDescriptor();
			m_Device->GetNativeDevice()->CreateRenderTargetView(*resource, NULL,descriptor.get_cpu_handle());
		}
		~RenderTargetView() {
			m_Device->GetDescriptorHeap<DescriptorHeapType::RTV>()->FreeDescriptor(descriptor);
			descriptor.invalidate();
		}
		RenderTargetView(RenderTargetView&& other) noexcept: ResourceView(std::move(other)) {};
	};
	struct DepthStencilView : public ResourceView<DepthStencilViewDesc> {
		inline Descriptor const& get_descriptor() { return descriptor; }
		DepthStencilView(Resource* resource, DepthStencilViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::DSV>()->AllocateDescriptor();
			const D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = desc;
			m_Device->GetNativeDevice()->CreateDepthStencilView(*resource, &dsvDesc,descriptor.get_cpu_handle());
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