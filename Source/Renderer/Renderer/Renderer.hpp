#pragma once
#include "../RHI/RHI.hpp"
#include "../RHI/TextureManager.hpp"
#include "../RHI/GeometryManager.hpp"
using namespace RHI;
class Renderer {	
	std::unique_ptr<Device> device;
	std::unique_ptr<TextureManager> textureManager;
	std::unique_ptr<GeometryManager> geometryManager;
	std::unique_ptr<Swapchain> swapChain;
	std::vector<std::unique_ptr<CommandList>> commandLists;
	std::vector<size_t> fenceValues;
	std::vector<std::unique_ptr<Fence>> fences;

	std::unique_ptr<ShaderBlob> m_MeshletAS;
	void beginFrame();
	void endFrame();
	void wait(CommandQueue* queue, Fence* fence);
	void wait(CommandListType type);
	bool bLoading = false;
public:
	bool bVsync = false;
	Renderer(Device::DeviceDesc const&, Swapchain::SwapchainDesc const&);
	void loadResources();
	void resizeViewport(UINT width, UINT height);
	void render();
	auto getDevice() { return device.get(); }
};