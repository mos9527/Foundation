#include "Renderer.hpp"
#include "../../IO/Image.hpp"
Renderer::Renderer(Device::DeviceDesc const& deviceCfg, Swapchain::SwapchainDesc const& swapchainCfg) {
	m_Device = std::make_unique<Device>(deviceCfg);	
	m_TextureMananger = std::make_unique<TextureManager>();
	m_GeometryMananger = std::make_unique<GeometryManager>(m_Device.get());
	m_Swapchain = std::make_unique<Swapchain>(m_Device.get(), m_Device->GetCommandQueue(CommandListType::Direct), swapchainCfg);
	
	m_CommandLists.resize(VALUE_OF(CommandListType::NUM_TYPES));
	m_Fences.resize(VALUE_OF(CommandListType::NUM_TYPES));
	m_FenceValues.resize(VALUE_OF(CommandListType::NUM_TYPES));

	m_CommandLists[VALUE_OF(CommandListType::Direct)] = std::make_unique<CommandList>(m_Device.get(), CommandListType::Direct, swapchainCfg.BackBufferCount); /* for n-buffering, n allocators are necessary to avoid premptive destruction of command lists */
	m_CommandLists[VALUE_OF(CommandListType::Direct)]->SetName(L"Direct Command List");
	m_Fences[VALUE_OF(CommandListType::Direct)] = std::make_unique<Fence>(m_Device.get());
	m_Fences[VALUE_OF(CommandListType::Direct)]->SetName(L"Direct Queue Fence");

	m_CommandLists[VALUE_OF(CommandListType::Copy)] = std::make_unique<CommandList>(m_Device.get(), CommandListType::Copy);
	m_CommandLists[VALUE_OF(CommandListType::Copy)]->SetName(L"Copy Command List");
	m_Fences[VALUE_OF(CommandListType::Copy)] = std::make_unique<Fence>(m_Device.get());
	m_Fences[VALUE_OF(CommandListType::Direct)]->SetName(L"Copy Queue Fence");
}
void Renderer::Wait(CommandQueue* queue, Fence* fence) {
	size_t fenceValue = queue->GetUniqueFenceValue();
	queue->Signal(fence, fenceValue);
	fence->Wait(fenceValue);
}
void Renderer::Wait(CommandListType type) {
	auto queue = m_Device->GetCommandQueue(type);	
	auto fence = m_Fences[VALUE_OF(type)].get();
	size_t fenceValue = queue->GetUniqueFenceValue();
	m_FenceValues[VALUE_OF(type)] = fenceValue;
	queue->Signal(fence, fenceValue);
	fence->Wait(fenceValue);
}
bool Renderer::IsFenceCompleted(CommandListType type) {
	auto fence = m_Fences[VALUE_OF(type)].get();
	return fence->IsCompleted(m_FenceValues[VALUE_OF(type)]);
}
void Renderer::LoadResources() {
	bLoading = true;
	auto cmdList = m_CommandLists[VALUE_OF(CommandListType::Copy)].get();
	Assimp::Importer importer;
	auto path = get_absolute_path("./x64/Resources/glTF-Sample-Models/2.0/DamagedHelmet/glTF/DamagedHelmet.gltf");
	auto scene = importer.ReadFile((const char*)path.u8string().c_str(), aiProcess_ConvertToLeftHanded);

	cmdList->Begin();
	m_GeometryMananger->LoadMeshes(m_Device.get(), cmdList, scene);
	cmdList->End();
	m_Device->GetCommandQueue(CommandListType::Copy)->Execute(cmdList);
	
	LOG(INFO) << "Executing copy";
	Wait(CommandListType::Copy);
	std::this_thread::sleep_for(std::chrono::seconds(4));
	LOG(INFO) << "Done";

	LOG(INFO) << "Pre GC";
	LogD3D12MABudget(m_Device->GetAllocator());
	LogD3D12MAPoolStatistics(m_Device->GetAllocatorPool(ResourcePoolType::Intermediate));
	m_Device->FlushIntermediateBuffers();
	LOG(INFO) << "Post GC";
	LogD3D12MABudget(m_Device->GetAllocator());
	LogD3D12MAPoolStatistics(m_Device->GetAllocatorPool(ResourcePoolType::Intermediate));
	bLoading = false;
}

void Renderer::ResizeViewport(UINT width, UINT height) {
	Wait(CommandListType::Direct);
	m_Swapchain->Resize(m_Device.get(), m_Device->GetCommandQueue(CommandListType::Direct), width, height);
	LOG(INFO) << "Viewport resized to " << width << 'x' << height;
}

void Renderer::BeginFrame() {
	m_CommandLists[VALUE_OF(CommandListType::Direct)]->ResetAllocator(m_Swapchain->GetCurrentBackbuffer());
	m_CommandLists[VALUE_OF(CommandListType::Direct)]->Begin(m_Swapchain->GetCurrentBackbuffer());
}

void Renderer::EndFrame() {
	m_CommandLists[VALUE_OF(CommandListType::Direct)]->End();
	m_Device->GetCommandQueue(CommandListType::Direct)->Execute(m_CommandLists[VALUE_OF(CommandListType::Direct)].get());
	m_Swapchain->PresentAndMoveToNextFrame(m_Device->GetCommandQueue(CommandListType::Direct), bVsync);
}

void Renderer::Render() {
	BeginFrame();
	// Populate command list
	auto cmdList = m_CommandLists[VALUE_OF(CommandListType::Direct)]->GetNativeCommandList();
	auto swapChain = m_Swapchain.get();
	auto bbIndex = swapChain->GetCurrentBackbuffer();
	
	// Indicate that the back buffer will be used as a render target.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET
	);
	cmdList->ResourceBarrier(1, &barrier);

	auto rtvHandle = swapChain->GetBackbufferRTV(bbIndex);
	cmdList->OMSetRenderTargets(1, &rtvHandle.cpu_handle, FALSE, nullptr);
	
	if (!bLoading) {
		const float clearColor[] = { 0.0f, 1.0f,0.0f, 1.0f };
		cmdList->ClearRenderTargetView(rtvHandle.cpu_handle, clearColor, 0, nullptr);
	}
	else {
		LOG(INFO) << "loading...";
		const float clearColor[] = { 1.0f, 0.0f,0.0f, 1.0f };
		cmdList->ClearRenderTargetView(rtvHandle.cpu_handle, clearColor, 0, nullptr);	
	}

	ID3D12DescriptorHeap* const heap = *m_Device->GetDescriptorHeap(DescriptorHeap::CBV_SRV_UAV);
	cmdList->SetDescriptorHeaps(1, &heap);

	cmdList->SetGraphicsRootSignature(m_Device->GetRootSignature());
	cmdList->SetGraphicsRoot32BitConstant(0, m_GeometryMananger->GetGeometryHandleBuffer()->GetGPUAddress(),0);

	// Indicate that the back buffer will now be used to present.
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT
	);
	cmdList->ResourceBarrier(1, &barrier);

	EndFrame();
}