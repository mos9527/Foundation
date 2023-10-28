#pragma once
#include "../RHI/RHI.hpp"
using namespace RHI;
class Renderer {	
	std::unique_ptr<Device> m_Device;
public:
	bool bVsync = true;
	void Init(Device::DeviceDesc const&, Swapchain::SwapchainDesc const&);
	void ResizeViewport(UINT width, UINT height);
	void BeginFrame();
	void RunFrame();
	void EndFrame();
	auto GetDevice() { return m_Device.get(); }
};