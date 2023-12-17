#include "D3D12Device.hpp"
#include "D3D12Resource.hpp"
#include "D3D12CommandList.hpp"

namespace RHI {
	template<typename Desc> struct ResourceView : public DeviceChild {
	protected:
		Descriptor descriptor;
	public:
		const Desc viewDesc;
		Resource* const resource;

		inline Desc const& GetDesc() { return viewDesc; }		
		ResourceView(Resource* resource, Desc const& viewDesc) : DeviceChild(resource->GetParent()), viewDesc(viewDesc), resource(resource) {};
		ResourceView(ResourceView const&) = delete;
		ResourceView(ResourceView&&) = default;
	};
	struct ShaderResourceView : public ResourceView<ShaderResourceViewDesc> {
	private:
		Descriptor persistent_descriptor;
	public:
		inline Descriptor const& get_storage_descriptor() { return descriptor;  }
		inline const Descriptor allocate_transient_descriptor(CommandListType ctxType) {
			const auto transitent = m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateTransitentDescriptor(ctxType);
			m_Device->GetNativeDevice()->CopyDescriptorsSimple(1, transitent.get_cpu_handle(), descriptor.get_cpu_handle(), DescriptorHeapTypeToD3DType(DescriptorHeapType::CBV_SRV_UAV));
			return transitent;
		}
		inline const Descriptor allocate_transient_descriptor(CommandList* ctx) {
			return allocate_transient_descriptor(ctx->GetType());
		}
		inline const Descriptor allocate_transient_descriptor(CommandQueue* ctx) {
			return allocate_transient_descriptor(ctx->GetType());
		}
		inline const Descriptor get_persistent_descriptor() {
			if (!persistent_descriptor.is_valid()) {
				persistent_descriptor = m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocatePersistentDescriptor();
				m_Device->GetNativeDevice()->CopyDescriptorsSimple(1, persistent_descriptor.get_cpu_handle(), descriptor.get_cpu_handle(), DescriptorHeapTypeToD3DType(DescriptorHeapType::CBV_SRV_UAV));
			}
			return persistent_descriptor;
		}
		inline void free_persistent_descriptor() {
			m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreePersistentDescriptor(persistent_descriptor);
			persistent_descriptor.invalidate();
		}
		ShaderResourceView(Resource* resource, ShaderResourceViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = desc;
			m_Device->GetNativeDevice()->CreateShaderResourceView(*resource, &srvDesc, descriptor.get_cpu_handle());
			return;
		}
		~ShaderResourceView() {
			m_Device->GetDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreeDescriptor(descriptor);
			m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreePersistentDescriptor(persistent_descriptor);
			descriptor.invalidate();
			persistent_descriptor.invalidate();
		}
		ShaderResourceView(ShaderResourceView&& other) noexcept: ResourceView(std::move(other)) {
			other.descriptor.invalidate();
			other.persistent_descriptor.invalidate();
		};
	};
	struct UnorderedAccessView : public ResourceView<UnorderedAccessViewDesc> {
	private:
		Descriptor persistent_descriptor;
	public:
		inline Descriptor const& get_storage_descriptor() { return descriptor; }
		inline const Descriptor allocate_transient_descriptor(CommandListType ctxType) {
			const auto transitent = m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateTransitentDescriptor(ctxType);
			m_Device->GetNativeDevice()->CopyDescriptorsSimple(1, transitent.get_cpu_handle(), descriptor.get_cpu_handle(), DescriptorHeapTypeToD3DType(DescriptorHeapType::CBV_SRV_UAV));
			return transitent;
		}
		inline const Descriptor allocate_transient_descriptor(CommandList* ctx) {
			return allocate_transient_descriptor(ctx->GetType());
		}
		inline const Descriptor allocate_transient_descriptor(CommandQueue* ctx) {
			return allocate_transient_descriptor(ctx->GetType());
		}
		inline const Descriptor get_persistent_descriptor() {
			if (!persistent_descriptor.is_valid()) {
				persistent_descriptor = m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocatePersistentDescriptor();
				m_Device->GetNativeDevice()->CopyDescriptorsSimple(1, persistent_descriptor.get_cpu_handle(), descriptor.get_cpu_handle(), DescriptorHeapTypeToD3DType(DescriptorHeapType::CBV_SRV_UAV));
			}
			return persistent_descriptor;
		}
		inline void free_persistent_descriptor() {
			m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreePersistentDescriptor(persistent_descriptor);
			persistent_descriptor.invalidate();
		}
		UnorderedAccessView(Resource* resource, Resource* counterResource, UnorderedAccessViewDesc const& desc) : ResourceView(resource, desc) {
			descriptor = m_Device->GetDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
			const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = desc;
			m_Device->GetNativeDevice()->CreateUnorderedAccessView(*resource, counterResource ? *counterResource : NULL, &uavDesc,descriptor.get_cpu_handle());
		}
		UnorderedAccessView(Resource* resource, UnorderedAccessViewDesc const& desc) : UnorderedAccessView(resource, nullptr, desc) {};
		~UnorderedAccessView() {
			m_Device->GetDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreeDescriptor(descriptor);
			m_Device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->FreePersistentDescriptor(persistent_descriptor);
			descriptor.invalidate();
			persistent_descriptor.invalidate();
		}
		UnorderedAccessView(UnorderedAccessView&& other) noexcept: ResourceView(std::move(other)) {
			other.descriptor.invalidate();
			other.persistent_descriptor.invalidate();
		};
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
		RenderTargetView(RenderTargetView&& other) noexcept: ResourceView(std::move(other)) {
			other.descriptor.invalidate();
		};
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
		DepthStencilView(DepthStencilView&& other) noexcept: ResourceView(std::move(other)) {
			other.descriptor.invalidate();
		};
	};
	struct Sampler : public ResourceView<SamplerDesc> {

	};	
}