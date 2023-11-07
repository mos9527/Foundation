#include "Renderer.hpp"
#include "../../IO/Image.hpp"
#include "Structs.hpp"

Renderer::Renderer(Device::DeviceDesc const& deviceCfg, Swapchain::SwapchainDesc const& swapchainCfg) {
	device = std::make_unique<Device>(deviceCfg);	
	// descriptor heaps that will be bound to the GPU are created here
	{
		using enum DescriptorHeapType;
		boundDescHeaps.resize(+NUM_TYPES);
		boundDescHeaps[+RTV] = std::make_unique<DescriptorHeap>(
			device.get(),
			DescriptorHeap::DescriptorHeapDesc{
			.shaderVisible = false,
				.descriptorCount = ALLOC_SIZE_BOUND_DESCHEAP,
				.heapType = RTV
		});
		boundDescHeaps[+RTV]->SetName(L"RTV Heap");
		boundDescHeaps[+DSV] = std::make_unique<DescriptorHeap>(
			device.get(),
			DescriptorHeap::DescriptorHeapDesc{
			.shaderVisible = true,
				.descriptorCount = ALLOC_SIZE_BOUND_DESCHEAP,
				.heapType = DSV
		});
		boundDescHeaps[+DSV]->SetName(L"DSV Heap");
		boundDescHeaps[+CBV_SRV_UAV] = std::make_unique<DescriptorHeap>(
			device.get(),
			DescriptorHeap::DescriptorHeapDesc{
			.shaderVisible = true,
				.descriptorCount = ALLOC_SIZE_BOUND_DESCHEAP,
				.heapType = CBV_SRV_UAV
		});
		boundDescHeaps[+CBV_SRV_UAV]->SetName(L"CBV-SRV-UAV Heap");
		storageDescHeap = std::make_unique<DescriptorHeap>(
			device.get(), 
			DescriptorHeap::DescriptorHeapDesc {
			.shaderVisible = false, /* MSDN: shader-visible descriptor heaps may be created in WRITE_COMBINE memory or GPU local memory, which is prohibitively slow to read from. */
				.descriptorCount = ALLOC_SIZE_STORE_DESCHEAP,
				.heapType = CBV_SRV_UAV
		});
		storageDescHeap->SetName(L"Storage Heap");
	}
	// swapchain
	swapChain = std::make_unique<Swapchain>(device.get(), device->GetCommandQueue(CommandListType::Direct), boundDescHeaps[+DescriptorHeapType::RTV].get(),swapchainCfg);
	// command lists
	{
		using enum CommandListType;
		commandLists.resize(+NUM_TYPES);
		fences.resize(+NUM_TYPES);
		fenceValues.resize(+NUM_TYPES);

		commandLists[+Direct] = std::make_unique<CommandList>(device.get(), Direct, swapchainCfg.BackBufferCount); /* for n-buffering, n allocators are necessary to avoid premptive destruction of command lists */
		commandLists[+Direct]->SetName(L"Direct Command List");
		fences[+Direct] = std::make_unique<Fence>(device.get());
		fences[+Direct]->SetName(L"Direct Queue Fence");

		commandLists[+Copy] = std::make_unique<CommandList>(device.get(), Copy);
		commandLists[+Copy]->SetName(L"Copy Command List");
		fences[+Copy] = std::make_unique<Fence>(device.get());
		fences[+Direct]->SetName(L"Copy Queue Fence");
	}
	resize_buffers(swapchainCfg.InitWidth, swapchainCfg.InitHeight);
	// scene buffers
	instanceBuffer.create(device.get(), INSTANCE_MAX_NUM_INSTANCES);
}
void Renderer::wait(CommandQueue* queue, Fence* fence) {
	size_t fenceValue = queue->GetUniqueFenceValue();
	queue->Signal(fence, fenceValue);
	fence->Wait(fenceValue);
}
void Renderer::wait(CommandListType type) {
	auto queue = device->GetCommandQueue(type);	
	auto fence = fences[+type].get();
	size_t fenceValue = queue->GetUniqueFenceValue();
	fenceValues[+type] = fenceValue;
	queue->Signal(fence, fenceValue);
	fence->Wait(fenceValue);
}
void Renderer::update_scene_buffer() {
	std::unordered_map<entt::entity, uint> geo_index_mapping;	
	uint instance_index{}, geo_index{};
	for (auto geo : registry.view<Geometry>()) {
		geo_index_mapping[geo] = geo_index++;
	}
	for (auto node : registry.view<SceneNode>()) {
		auto& geo = registry.get<Geometry>(node);		
		InstanceData instance;
		instance.instance_index = instance_index++;
		instance.geometry_index = geo_index_mapping[node];
		instance.material_index = -1; // xxx
		instance.obb_center = geo.aabb.Center;
		instance.obb_extends = geo.aabb.Extents;
		instance.transform = Matrix::Identity;
	}
}
void Renderer::load_resources() {
	bLoading = true;
	// shaders
	auto data = IO::read_data(IO::get_absolute_path("MeshletAS.cso"));
	testAS = std::make_unique<ShaderBlob>(data->data(), data->size());
	data = IO::read_data(IO::get_absolute_path("MeshletMS.cso"));
	testMS = std::make_unique<ShaderBlob>(data->data(), data->size());
	data = IO::read_data(IO::get_absolute_path("MeshletPS.cso"));
	testPS = std::make_unique<ShaderBlob>(data->data(), data->size());	
	// root sig
	testRootSig = std::make_unique<RootSignature>(device.get(), testAS.get());
	// pso
	D3DX12_MESH_SHADER_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = testRootSig->GetNativeRootSignature();
	psoDesc.AS = { testAS->GetData(), testAS->GetSize() };
	psoDesc.MS = { testMS->GetData(), testMS->GetSize() };
	psoDesc.PS = { testPS->GetData(), testPS->GetSize() };
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = swapChain->GetBackbuffer(0)->GetNativeBuffer()->GetDesc().Format;
	psoDesc.DSVFormat = ResourceFormatToD3DFormat(testDepthStencil->GetDesc().format);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);      // CW front; cull back
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);                // Opaque
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); // Less-equal depth test w/ writes; no stencil
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.SampleDesc = DefaultSampleDesc();

	auto meshStreamDesc = CD3DX12_PIPELINE_MESH_STATE_STREAM(psoDesc);
	// Point to our populated stream desc
	D3D12_PIPELINE_STATE_STREAM_DESC streamDesc = {};
	streamDesc.SizeInBytes = sizeof(meshStreamDesc);
	streamDesc.pPipelineStateSubobjectStream = &meshStreamDesc;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pso)));
	testPso = std::make_unique<PipelineState>(std::move(pso));
	{
		using enum CommandListType;
		auto cmdList = commandLists[+Copy].get();

		Assimp::Importer importer;
		auto path = IO::get_absolute_path("../Resources/glTF-Sample-Models/2.0/DamagedHelmet/glTF/DamagedHelmet.gltf");
		auto import_scene = importer.ReadFile((const char*)path.u8string().c_str(), aiProcess_ConvertToLeftHanded);
		
		cmdList->Begin();
		scene.load_from_aiScene(device.get(), cmdList, import_scene);
		cmdList->End();

		LOG(INFO) << "Executing copy";
		device->GetCommandQueue(Copy)->Execute(cmdList);
		wait(Copy);
		LOG(INFO) << "Done";
	}
	{
		using enum ResourcePoolType;
		LOG(INFO) << "Pre GC";
		LogD3D12MABudget(device->GetAllocator());
		LogD3D12MAPoolStatistics(device->GetAllocatorPool(Intermediate));
		device->FlushIntermediateBuffers();
		LOG(INFO) << "Post GC";
		LogD3D12MABudget(device->GetAllocator());
		LogD3D12MAPoolStatistics(device->GetAllocatorPool(Intermediate));
		bLoading = false;
	}
}

void Renderer::resize_buffers(UINT width, UINT height) {
	testDepthStencil = std::make_unique<RHI::Texture>(device.get(), RHI::Texture::TextureDesc::GetTextureBufferDesc(
		ResourceFormat::R32_FLOAT,
		ResourceDimension::Texture2D,
		width,
		height,
		1, 1, 1, 0,
		ResourceFlags::DepthStencil
	));
}
void Renderer::resize_viewport(UINT width, UINT height) {
	wait(CommandListType::Direct);
	swapChain->Resize(device.get(), device->GetCommandQueue(CommandListType::Direct), boundDescHeaps[+DescriptorHeapType::RTV].get(), width, height);
	LOG(INFO) << "Viewport resized to " << width << 'x' << height;
}

void Renderer::begin_frame() {
	using enum CommandListType;
	commandLists[+Direct]->ResetAllocator(swapChain->GetCurrentBackbufferIndex());
	commandLists[+Direct]->Begin(swapChain->GetCurrentBackbufferIndex());
}

void Renderer::end_frame() {
	using enum CommandListType;
	commandLists[+Direct]->End();
	device->GetCommandQueue(Direct)->Execute(commandLists[+Direct].get());
	swapChain->PresentAndMoveToNextFrame(device->GetCommandQueue(Direct), bVsync);
}

void Renderer::render() {
	begin_frame();
	// Populate command list
	using enum CommandListType;
	auto cmdList = commandLists[+Direct]->GetNativeCommandList();
	auto bbIndex = swapChain->GetCurrentBackbufferIndex();
	
	// Indicate that the back buffer will be used as a render target.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET
	);
	cmdList->ResourceBarrier(1, &barrier);

	auto rtvHandle = swapChain->GetBackbufferRTV(bbIndex);
	cmdList->OMSetRenderTargets(1, &rtvHandle->get_cpu_handle(), FALSE, nullptr);

	if (bLoading) {
		const float clearColor[] = { 1.0f, 0.0f,0.0f, 1.0f };
		cmdList->ClearRenderTargetView(rtvHandle->get_cpu_handle(), clearColor, 0, nullptr);
	}
	else {
		const float clearColor[] = { 0.0f, 1.0f,0.0f, 1.0f };
		cmdList->ClearRenderTargetView(rtvHandle->get_cpu_handle(), clearColor, 0, nullptr);
		auto srvHeap = boundDescHeaps[+DescriptorHeapType::CBV_SRV_UAV]->GetNativeHeap();
		cmdList->SetGraphicsRootSignature(*testRootSig);
		cmdList->SetDescriptorHeaps(1, &srvHeap);
		cmdList->SetGraphicsRoot32BitConstant(0, 0, 0); // b0[0] geo handle buffer
		cmdList->SetPipelineState(*testPso);
		cmdList->DispatchMesh(1, 1, 1); // testing : draw one geo

	}
	// Indicate that the back buffer will now be used to present.
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT
	);
	cmdList->ResourceBarrier(1, &barrier);
	end_frame();
}