#pragma once
#include "../RHI/RHI.hpp"
using namespace RHI;
class Renderer {	
	std::unique_ptr<Device> device;
	std::unique_ptr<Swapchain> swapChain;

	std::vector<std::unique_ptr<CommandList>> commandLists;
	std::vector<size_t> fenceValues;
	std::vector<std::unique_ptr<Fence>> fences;

	/* testing */
	std::unique_ptr<Texture> testDepthStencil;
	std::unique_ptr<PipelineState> testPso;
	std::unique_ptr<ShaderBlob> testAS, testMS, testPS;
	std::unique_ptr<RootSignature> testRootSig;

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