#include "Renderer.hpp"
#include "../../IO/Image.hpp"
Renderer::Renderer(Device::DeviceDesc const& deviceCfg, Swapchain::SwapchainDesc const& swapchainCfg) {
	m_Device = std::make_unique<Device>(deviceCfg);
	m_Device->CreateSwapchainAndBackbuffers(swapchainCfg);	
	m_TextureMananger = std::make_unique<TextureManager>();
	m_GeometryMananger = std::make_unique<GeometryManager>(m_Device.get());
}

void Renderer::LoadResources() {
	auto cmdList = m_Device->GetCommandList(CommandList::DIRECT);
	Assimp::Importer importer;
	auto path = get_absolute_path("./x64/Resources/glTF-Sample-Models/2.0/DamagedHelmet/glTF/DamagedHelmet.gltf");
	auto scene = importer.ReadFile((const char*)path.u8string().c_str(), aiProcess_ConvertToLeftHanded);

	cmdList->Begin();
	m_GeometryMananger->LoadMeshes(m_Device.get(), cmdList, scene);
	cmdList->End();
	m_Device->GetCommandQueue()->Execute(cmdList);
	m_Device->Wait();
	
	LOG(INFO) << "Pre GC";
	LogD3D12MABudget(m_Device->GetAllocator());
	
	m_Device->FlushIntermediateBuffers();
	LOG(INFO) << "Post GC";
	LogD3D12MABudget(m_Device->GetAllocator());
}

void Renderer::ResizeViewport(UINT width, UINT height) {
	m_Device->Wait();
	m_Device->GetSwapchain()->Resize(m_Device.get(), width, height);
	LOG(INFO) << "Viewport resized to " << width << 'x' << height;
}

void Renderer::Render() {
	m_Device->BeginFrame();
	// Populate command list
	auto cmdList = m_Device->GetCommandList(CommandList::DIRECT)->operator ID3D12GraphicsCommandList6 * ();
	auto swapChain = m_Device->GetSwapchain();
	auto bbIndex = swapChain->nBackbufferIndex;
	
	// Indicate that the back buffer will be used as a render target.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET
	);
	cmdList->ResourceBarrier(1, &barrier);

	auto rtvHandle = swapChain->GetBackbufferRTV(bbIndex);
	cmdList->OMSetRenderTargets(1, &rtvHandle.cpu_handle, FALSE, nullptr);

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	cmdList->ClearRenderTargetView(rtvHandle.cpu_handle, clearColor, 0, nullptr);

	ID3D12DescriptorHeap* const heap = *m_Device->GetDescriptorHeap(DescriptorHeap::CBV_SRV_UAV);
	cmdList->SetDescriptorHeaps(1, &heap);

	cmdList->SetGraphicsRootShaderResourceView(0, m_GeometryMananger->GetGeometryHandleBuffer()->GetGPUAddress());
	
	cmdList->ExecuteIndirect(
		*m_Device->GetCommandSignature(CommandSignature::IndirectArgumentType::DISPATCH_MESH),
		1,
		NULL,
		0,
		NULL,
		0
	);

	// Indicate that the back buffer will now be used to present.
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT
	);
	cmdList->ResourceBarrier(1, &barrier);

	m_Device->EndFrame(bVsync);
}