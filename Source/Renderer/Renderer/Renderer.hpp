#pragma once
#include "../RHI/RHI.hpp"
using namespace RHI;
class Renderer {
	std::unique_ptr<Device> m_Device;
public:
	void Init(Device::DeviceConfig const&, Swapchain::SwapchainConfig const&);
	void BeginFrame();
	void RunFrame();
	void EndFrame();
	auto GetDevice() { return m_Device.get(); }
};