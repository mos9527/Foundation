#include "Renderer.hpp"
#include "../../IO/Image.hpp"
#include "Structs.hpp"

Renderer::Renderer(Device::DeviceDesc const& deviceCfg, Swapchain::SwapchainDesc const& swapchainCfg) {
	device = std::make_unique<Device>(deviceCfg);	
	// descriptor heaps that will be bound to the GPU are created here
	{
		using enum DescriptorHeapType;
		boundDescHeaps.resize(+NUM_TYPES);
		boundDescHeaps[+RTV] = std::make_unique<DescriptorHeap>(
			device.get(),
			DescriptorHeap::DescriptorHeapDesc{
			.shaderVisible = false,
				.descriptorCount = ALLOC_SIZE_BOUND_DESCHEAP,
				.heapType = RTV
		});
		boundDescHeaps[+RTV]->SetName(L"RTV Heap");
		boundDescHeaps[+DSV] = std::make_unique<DescriptorHeap>(
			device.get(),
			DescriptorHeap::DescriptorHeapDesc{
			.shaderVisible = false,
				.descriptorCount = ALLOC_SIZE_BOUND_DESCHEAP,
				.heapType = DSV
		});
		boundDescHeaps[+DSV]->SetName(L"DSV Heap");
		boundDescHeaps[+CBV_SRV_UAV] = std::make_unique<DescriptorHeap>(
			device.get(),
			DescriptorHeap::DescriptorHeapDesc{
			.shaderVisible = true,
				.descriptorCount = ALLOC_SIZE_BOUND_DESCHEAP,
				.heapType = CBV_SRV_UAV
		});
		boundDescHeaps[+CBV_SRV_UAV]->SetName(L"CBV-SRV-UAV Heap");
		storageDescHeap = std::make_unique<DescriptorHeap>(
			device.get(), 
			DescriptorHeap::DescriptorHeapDesc {
			.shaderVisible = false, /* MSDN: shader-visible descriptor heaps may be created in WRITE_COMBINE memory or GPU local memory, which is prohibitively slow to read from. */
				.descriptorCount = ALLOC_SIZE_STORE_DESCHEAP,
				.heapType = CBV_SRV_UAV
		});
		storageDescHeap->SetName(L"Storage Heap");
	}
	// swapchain
	swapChain = std::make_unique<Swapchain>(device.get(), device->GetCommandQueue(CommandListType::Direct), boundDescHeaps[+DescriptorHeapType::RTV].get(),swapchainCfg);
	// command lists
	{
		using enum CommandListType;
		commandLists.resize(+NUM_TYPES);
		fences.resize(+NUM_TYPES);
		fenceValues.resize(+NUM_TYPES);

		commandLists[+Direct] = std::make_unique<CommandList>(device.get(), Direct, swapchainCfg.BackBufferCount); /* for n-buffering, n allocators are necessary to avoid premptive destruction of command lists */
		commandLists[+Direct]->SetName(L"Direct Command List");
		fences[+Direct] = std::make_unique<Fence>(device.get());
		fences[+Direct]->SetName(L"Direct Queue Fence");

		commandLists[+Copy] = std::make_unique<CommandList>(device.get(), Copy);
		commandLists[+Copy]->SetName(L"Copy Command List");
		fences[+Copy] = std::make_unique<Fence>(device.get());
		fences[+Direct]->SetName(L"Copy Queue Fence");
	}
}

entt::entity Renderer::allocate_buffer(Buffer::BufferDesc const& desc) {
	auto entity = registry.create();
	registry.emplace<Buffer>(entity, device.get(), desc);
	return entity;
}

entt::entity Renderer::allocate_texture(Texture::TextureDesc const& desc) {
	auto entity = registry.create();
	registry.emplace<Texture>(entity, device.get(), desc);
	return entity;
};