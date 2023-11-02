#include "Renderer.hpp"
#include "../../IO/Image.hpp"
Renderer::Renderer(Device::DeviceDesc const& deviceCfg, Swapchain::SwapchainDesc const& swapchainCfg) {
	device = std::make_unique<Device>(deviceCfg);	
	textureManager = std::make_unique<TextureManager>();
	geometryManager = std::make_unique<GeometryManager>(device.get());
	swapChain = std::make_unique<Swapchain>(device.get(), device->GetCommandQueue(CommandListType::Direct), swapchainCfg);
	
	commandLists.resize(VALUE_OF(CommandListType::NUM_TYPES));
	fences.resize(VALUE_OF(CommandListType::NUM_TYPES));
	fenceValues.resize(VALUE_OF(CommandListType::NUM_TYPES));

	commandLists[VALUE_OF(CommandListType::Direct)] = std::make_unique<CommandList>(device.get(), CommandListType::Direct, swapchainCfg.BackBufferCount); /* for n-buffering, n allocators are necessary to avoid premptive destruction of command lists */
	commandLists[VALUE_OF(CommandListType::Direct)]->SetName(L"Direct Command List");
	fences[VALUE_OF(CommandListType::Direct)] = std::make_unique<Fence>(device.get());
	fences[VALUE_OF(CommandListType::Direct)]->SetName(L"Direct Queue Fence");

	commandLists[VALUE_OF(CommandListType::Copy)] = std::make_unique<CommandList>(device.get(), CommandListType::Copy);
	commandLists[VALUE_OF(CommandListType::Copy)]->SetName(L"Copy Command List");
	fences[VALUE_OF(CommandListType::Copy)] = std::make_unique<Fence>(device.get());
	fences[VALUE_OF(CommandListType::Direct)]->SetName(L"Copy Queue Fence");
}
void Renderer::wait(CommandQueue* queue, Fence* fence) {
	size_t fenceValue = queue->GetUniqueFenceValue();
	queue->Signal(fence, fenceValue);
	fence->Wait(fenceValue);
}
void Renderer::wait(CommandListType type) {
	auto queue = device->GetCommandQueue(type);	
	auto fence = fences[VALUE_OF(type)].get();
	size_t fenceValue = queue->GetUniqueFenceValue();
	fenceValues[VALUE_OF(type)] = fenceValue;
	queue->Signal(fence, fenceValue);
	fence->Wait(fenceValue);
}
void Renderer::loadResources() {
	bLoading = true;
	// shaders...
	auto as = read_data(get_absolute_path("MeshletAS.cso"));
	m_MeshletAS = std::make_unique<ShaderBlob>(as->data(), as->size());

	auto cmdList = commandLists[VALUE_OF(CommandListType::Copy)].get();
	Assimp::Importer importer;
	auto path = get_absolute_path("./x64/Resources/glTF-Sample-Models/2.0/DamagedHelmet/glTF/DamagedHelmet.gltf");
	auto scene = importer.ReadFile((const char*)path.u8string().c_str(), aiProcess_ConvertToLeftHanded);

	cmdList->Begin();
	geometryManager->LoadMeshes(device.get(), cmdList, scene);
	cmdList->End();
	device->GetCommandQueue(CommandListType::Copy)->Execute(cmdList);
	
	LOG(INFO) << "Executing copy";
	wait(CommandListType::Copy);
	LOG(INFO) << "Done";

	LOG(INFO) << "Pre GC";
	LogD3D12MABudget(device->GetAllocator());
	LogD3D12MAPoolStatistics(device->GetAllocatorPool(ResourcePoolType::Intermediate));
	device->FlushIntermediateBuffers();
	LOG(INFO) << "Post GC";
	LogD3D12MABudget(device->GetAllocator());
	LogD3D12MAPoolStatistics(device->GetAllocatorPool(ResourcePoolType::Intermediate));
	bLoading = false;
}

void Renderer::resizeViewport(UINT width, UINT height) {
	wait(CommandListType::Direct);
	swapChain->Resize(device.get(), device->GetCommandQueue(CommandListType::Direct), width, height);
	LOG(INFO) << "Viewport resized to " << width << 'x' << height;
}

void Renderer::beginFrame() {
	commandLists[VALUE_OF(CommandListType::Direct)]->ResetAllocator(swapChain->GetCurrentBackbuffer());
	commandLists[VALUE_OF(CommandListType::Direct)]->Begin(swapChain->GetCurrentBackbuffer());
}

void Renderer::endFrame() {
	commandLists[VALUE_OF(CommandListType::Direct)]->End();
	device->GetCommandQueue(CommandListType::Direct)->Execute(commandLists[VALUE_OF(CommandListType::Direct)].get());
	swapChain->PresentAndMoveToNextFrame(device->GetCommandQueue(CommandListType::Direct), bVsync);
}

void Renderer::render() {
	beginFrame();
	// Populate command list
	auto cmdList = commandLists[VALUE_OF(CommandListType::Direct)]->GetNativeCommandList();
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
		const float clearColor[] = { 1.0f, 0.0f,0.0f, 1.0f };
		cmdList->ClearRenderTargetView(rtvHandle.cpu_handle, clearColor, 0, nullptr);	
	}

	ID3D12DescriptorHeap* const heap = *device->GetDescriptorHeap(DescriptorHeap::CBV_SRV_UAV);
	cmdList->SetDescriptorHeaps(1, &heap);

	cmdList->SetGraphicsRootSignature(device->GetRootSignature());
	cmdList->SetGraphicsRoot32BitConstant(0, geometryManager->GetGeometryHandleBuffer()->GetGPUAddress(),0);

	// Indicate that the back buffer will now be used to present.
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT
	);
	cmdList->ResourceBarrier(1, &barrier);

	endFrame();
}