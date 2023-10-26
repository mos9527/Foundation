#include "Renderer.hpp"
void Renderer::Init(Device::DeviceConfig const& deviceCfg, Swapchain::SwapchainConfig const& swapchainCfg) {
	m_Device = std::make_unique<Device>(deviceCfg);
	m_Device->CreateSwapchainAndBackbuffers(swapchainCfg);
}

void Renderer::BeginFrame() {
	m_Device->BeginFrame();
}

void Renderer::EndFrame() {
	m_Device->EndFrame();
}