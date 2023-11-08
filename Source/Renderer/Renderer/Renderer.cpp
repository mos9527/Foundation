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
	using enum DescriptorHeapType;
	instanceBuffer.create(device.get(), boundDescHeaps[+CBV_SRV_UAV].get(), INSTANCE_MAX_NUM_INSTANCES, L"Instance Buffer");
	geometryBuffer.create(device.get(), boundDescHeaps[+CBV_SRV_UAV].get(), INSTANCE_MAX_NUM_INSTANCES, L"Geometry Index Buffer");
	cameraCBV.create(device.get(), boundDescHeaps[+CBV_SRV_UAV].get(), L"Camera");
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
	auto bound_heap = boundDescHeaps[+DescriptorHeapType::CBV_SRV_UAV].get();	
	for (auto ent : registry.view<Geometry>()) {
		auto& geo = registry.get<Geometry>(ent);
		if (!geo.geometry_bound_descriptor) {
			geo.geometry_bound_descriptor = bound_heap->AllocateDescriptor();
			geo.geometry_properties.heap_index = geo.geometry_bound_descriptor->get_heap_handle();
		}
		bound_heap->CopyInto(device.get(), geo.geometry_storage_descriptor.get(), geo.geometry_bound_descriptor.get());		
		geo_index_mapping[ent] = geo_index;
		geometryBuffer.update_upload_buffer(&geo.geometry_properties, geo_index);
		geo_index++;
	}
	numGeometry = geo_index;
	for (auto ent : registry.view<SceneNode>()) { 
		auto& geo = registry.get<Geometry>(ent);
		InstanceData instance;
		instance.instance_index = instance_index++;
		instance.geometry_index = geo_index_mapping[ent];
		instance.material_index = -1; // xxx
		instance.transform = Matrix::Identity;		
		BoundingSphere aabb_sphere; // sphere aabb for LOD calculation
		BoundingSphere::CreateFromBoundingBox(aabb_sphere, geo.aabb);
		instance.aabb_sphere_center_radius.x = aabb_sphere.Center.x;
		instance.aabb_sphere_center_radius.y = aabb_sphere.Center.y;
		instance.aabb_sphere_center_radius.z = aabb_sphere.Center.z;
		instance.aabb_sphere_center_radius.w = aabb_sphere.Radius;
		instanceBuffer.update_upload_buffer(&instance, instance_index);
	}
	numInstances = instance_index;
	cameraCBV.update();
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
	// root sig pulled from AS
	testRootSig = std::make_unique<RootSignature>(device.get(), testAS.get());
	// pso
	D3DX12_MESH_SHADER_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = testRootSig->GetNativeRootSignature();
	psoDesc.AS = { testAS->GetData(), testAS->GetSize() };
	psoDesc.MS = { testMS->GetData(), testMS->GetSize() };
	psoDesc.PS = { testPS->GetData(), testPS->GetSize() };
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = swapChain->GetBackbuffer(0)->GetNativeBuffer()->GetDesc().Format;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
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
		
		LOG(INFO) << "Loading...";		
		cmdList->Begin();
		scene.load_from_aiScene(device.get(), cmdList, storageDescHeap.get(), import_scene);
		instanceBuffer.update_dest_buffer(cmdList);
		geometryBuffer.update_dest_buffer(cmdList);
		cmdList->End();
		LOG(INFO) << "Uploading...";
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
	testDepthStencil.resize(device.get(), boundDescHeaps[+DescriptorHeapType::DSV].get(), width, height);
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
	// Populate gfx command list
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
	cmdList->OMSetRenderTargets(1, &rtvHandle->get_cpu_handle(), FALSE, &testDepthStencil.depthStencilView->get_cpu_handle());	
	if (bLoading) {
		const float clearColor[] = { 1.0f, 0.0f,0.0f, 1.0f };
		cmdList->ClearRenderTargetView(rtvHandle->get_cpu_handle(), clearColor, 0, nullptr);
	}
	else {
		const float clearColor[] = { 0.0f, 1.0f,0.0f, 1.0f };
		cmdList->ClearRenderTargetView(rtvHandle->get_cpu_handle(), clearColor, 0, nullptr);
		cmdList->ClearDepthStencilView(testDepthStencil.depthStencilView->get_cpu_handle(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		auto srvHeap = boundDescHeaps[+DescriptorHeapType::CBV_SRV_UAV]->GetNativeHeap();
		cmdList->SetGraphicsRootSignature(*testRootSig);
		cmdList->SetDescriptorHeaps(1, &srvHeap);
		// b0 : bindless indices
		cmdList->SetGraphicsRoot32BitConstant(0, instanceBuffer.dest_bound_desc->get_heap_handle(), 0);
		cmdList->SetGraphicsRoot32BitConstant(0, geometryBuffer.dest_bound_desc->get_heap_handle(), 1);
		cmdList->SetGraphicsRoot32BitConstant(0, cameraCBV.bound_desc->get_heap_handle(), 2);		
		cmdList->SetPipelineState(*testPso);
		
		uint numDispatches = DivRoundUp(numInstances, DISPATCH_GROUP_COUNT);
		for (uint i = 0; i < numDispatches; i++) {
			uint offset = numDispatches * i;
			uint count = std::min(numInstances - offset, numDispatches);
			cmdList->SetGraphicsRoot32BitConstant(1, offset, 0);
			cmdList->SetGraphicsRoot32BitConstant(1, count, 1);
			cmdList->DispatchMesh(count, 1, 1);
		}
	}
	// Indicate that the back buffer will now be used to present.
	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		*swapChain->GetBackbuffer(bbIndex),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT
	);
	cmdList->ResourceBarrier(1, &barrier);
	end_frame();
}