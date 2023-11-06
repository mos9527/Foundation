#pragma once
#include "../RHI/RHI.hpp"
#include "../Scene/Scene.hpp"
using namespace RHI;
class Renderer {	
	std::unique_ptr<Device> device;
	std::unique_ptr<Swapchain> swapChain;
	
	std::vector<std::unique_ptr<DescriptorHeap>> boundDescHeaps;
	ID3D12DescriptorHeap* boundDescHeapsD3D[+DescriptorHeapType::NUM_TYPES];
	// storage descriptor heap for storing maybe-used CBV-SRV-UAV descriptors
	// which should be copied at runtime when necessary
	std::unique_ptr<DescriptorHeap> storageDescHeap;

	std::vector<std::unique_ptr<CommandList>> commandLists;
	std::vector<size_t> fenceValues;
	std::vector<std::unique_ptr<Fence>> fences;

	/* testing */
	std::unique_ptr<RHI::Texture> testDepthStencil;
	std::unique_ptr<PipelineState> testPso;
	std::unique_ptr<ShaderBlob> testAS, testMS, testPS;
	std::unique_ptr<RootSignature> testRootSig;

	SceneGraph scene;

	void begin_frame();
	void end_frame();
	void wait(CommandQueue* queue, Fence* fence);
	void wait(CommandListType type);
	void resize_buffers(UINT width, UINT height);
	bool bLoading = false;
public:
	bool bVsync = false;
	Renderer(Device::DeviceDesc const&, Swapchain::SwapchainDesc const&);
	void load_resources();
	void resize_viewport(UINT width, UINT height);
	void render();
	auto getDevice() { return device.get(); }
};