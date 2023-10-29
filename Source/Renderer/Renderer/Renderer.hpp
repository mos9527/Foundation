#pragma once
#include "../RHI/RHI.hpp"
#include "../RHI/TextureManager.hpp"
#include "../RHI/GeometryManager.hpp"
using namespace RHI;
class Renderer {	
	std::unique_ptr<Device> m_Device;
	std::unique_ptr<TextureManager> m_TextureMananger;
	std::unique_ptr<GeometryManager> m_GeometryMananger;
public:
	bool bVsync = true;
	Renderer(Device::DeviceDesc const&, Swapchain::SwapchainDesc const&);
	void LoadResources();
	void ResizeViewport(UINT width, UINT height);
	void Render();
	auto GetDevice() { return m_Device.get(); }
};