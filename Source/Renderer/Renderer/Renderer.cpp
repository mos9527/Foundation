#include "Renderer.hpp"
void Renderer::Init(Device::DeviceDesc const& deviceCfg, Swapchain::SwapchainDesc const& swapchainCfg) {
	m_Device = std::make_unique<Device>(deviceCfg);
	m_Device->CreateSwapchainAndBackbuffers(swapchainCfg);	
}

void Renderer::ResizeViewport(UINT width, UINT height) {
	m_Device->WaitForGPU();
	m_Device->GetSwapchain()->Resize(m_Device.get(), width, height);
	LOG(INFO) << "Viewport resized to " << width << 'x' << height;
}

void Renderer::BeginFrame() {
	m_Device->BeginFrame();
}

void Renderer::EndFrame() {
	m_Device->EndFrame(bVsync);
}