#include "Renderer.hpp"
#include "../../IO/Image.hpp"
Renderer::Renderer(Device::DeviceDesc const& deviceCfg, Swapchain::SwapchainDesc const& swapchainCfg) {
	m_Device = std::make_unique<Device>(deviceCfg);
	m_Device->CreateSwapchainAndBackbuffers(swapchainCfg);	
	m_TextureMananger = std::make_unique<TextureManager>();
}

void Renderer::LoadResources() {
	auto cmdList = m_Device->GetCommandList(CommandList::DIRECT);
	auto bmp = IO::LoadBitmap8bpp("./x64/Resources/glTF-Sample-Models/2.0/DamagedHelmet/glTF/Default_albedo.jpg");
	cmdList->Begin();
	m_TextureMananger->LoadTexture(m_Device.get(), cmdList, bmp);
	cmdList->End();
	m_Device->GetCommandQueue()->Execute(cmdList);
	m_Device->WaitForGPU();
	LOG(INFO) << "Loaded image";
	m_Device->FlushIntermediateBuffers(); // Manual GC
}

void Renderer::ResizeViewport(UINT width, UINT height) {
	m_Device->WaitForGPU();
	m_Device->GetSwapchain()->Resize(m_Device.get(), width, height);
	LOG(INFO) << "Viewport resized to " << width << 'x' << height;
}

void Renderer::Render() {
	m_Device->BeginFrame();
	// Populate command list
	auto D3DcmdList = m_Device->GetCommandList(CommandList::DIRECT)->operator ID3D12GraphicsCommandList6 * ();
	auto swapChain = m_Device->GetSwapchain();
	auto bbIndex = swapChain->nBackbufferIndex;

	// Indicate that the back buffer will be used as a render target.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET
	);
	D3DcmdList->ResourceBarrier(1, &barrier);

	auto rtvHandle = swapChain->GetBackbufferRTV(bbIndex);
	D3DcmdList->OMSetRenderTargets(1, &rtvHandle.cpu_handle, FALSE, nullptr);

	const float clearColor[] = { rand() / RAND_MAX, 0.2f, 0.4f, 1.0f };
	D3DcmdList->ClearRenderTargetView(rtvHandle.cpu_handle, clearColor, 0, nullptr);

	// Indicate that the back buffer will now be used to present.
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT
	);
	D3DcmdList->ResourceBarrier(1, &barrier);

	m_Device->EndFrame(bVsync);
}