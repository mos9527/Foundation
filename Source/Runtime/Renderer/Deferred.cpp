#include "Deferred.hpp"
#include "../AssetRegistry/IO.hpp"
using namespace RHI;
void DeferredRenderer::ResetCounter(CommandList* cmd, Resource* resource, size_t offset) {
	cmd->CopyBufferRegion(resetBuffer.get(), resource, 0, offset, sizeof(UINT));
}
DeferredRenderer::DeferredRenderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapchain) : Renderer(assets, scene, device, swapchain) {
	sceneView = std::make_unique<SceneGraphView>(device, scene);
	// cull pass
	cullPassCS = std::make_unique<Shader>(L"Shaders/InstanceCull.hlsl", L"main", L"cs_6_6");
	cullPassRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()			
			.AddConstantBufferView(0,0) // c0 s0 : SceneGlobals	
			.AddShaderResourceView(0,0) // t0 s0 : SceneMeshInstance[s]
			.AddUnorderedAccessViewWithCounter(0,0)// u0 s0 : Indirect Commandlists
	);
	cullPassRS->SetName(L"Indirect Cull & LOD Classification");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *cullPassRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cullPassCS->GetData(), cullPassCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	cullPassPSO = std::make_unique<PipelineState>(device, std::move(pso));
	// gbuffer pass
	gBufferPS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"ps_main", L"ps_6_6");
	gBufferVS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"vs_main", L"vs_6_6");
	gBufferRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetAllowInputAssembler()
		.SetDirectlyIndexed() // xxx textures?
		.AddConstantBufferView(0, 0) // c0 s0 : SceneGlobals			
	);
	gBufferRS->SetName(L"GBuffer generation");
	// Define the vertex input layout.
	auto iaLayout = VertexLayoutToD3DIADesc(StaticMeshAsset::Vertex::get_layout());
	// Describe and create the graphics pipeline state objects (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferPsoDesc = {};
	gbufferPsoDesc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	gbufferPsoDesc.pRootSignature = *gBufferRS;
	gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(gBufferVS->GetData(), gBufferVS->GetSize());
	gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(gBufferPS->GetData(), gBufferPS->GetSize());
	gbufferPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	gbufferPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	gbufferPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
#ifdef INVERSE_Z
	gbufferPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
	gbufferPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	gbufferPsoDesc.SampleMask = UINT_MAX;
	gbufferPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	gbufferPsoDesc.NumRenderTargets = 1;
	gbufferPsoDesc.RTVFormats[0] = ResourceFormatToD3DFormat(swapchain->GetFormat());
	gbufferPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	gbufferPsoDesc.SampleDesc.Count = 1;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&pso)));
	gBufferPSO = std::make_unique<PipelineState>(device, std::move(pso));
	// indirect command buffer
	gBufferIndirectCommandSig = std::make_unique<CommandSignature>(
		device,
		nullptr,
		CommandSignatureDesc(sizeof(IndirectCommand))
			.AddVertexBufferView(0)
			.AddIndexBufferView()
			.AddDrawIndexed()			
	);
	indirectCmdBuffer = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		CommandBufferSize, sizeof(IndirectCommand), ResourceState::CopyDest, ResourceHeapType::Default, ResourceFlags::UnorderedAccess
	));
	indirectCmdBuffer->SetName(L"Indirect Commands Buffer");
	indirectCmdBufferUAV = std::make_unique<UnorderedAccessView>(
		indirectCmdBuffer.get(), indirectCmdBuffer.get(),  UnorderedAccessViewDesc::GetStructuredBufferDesc(
			0, MAX_INSTANCE_COUNT, sizeof(IndirectCommand), CommandBufferCounterOffset
		)
	);
	resetBuffer = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(sizeof(uint), sizeof(uint)));
	resetBuffer->SetName(L"Zero buffer");
	uint resetValue = 0;
	resetBuffer->Update(&resetValue, sizeof(resetValue), 0);
};
void DeferredRenderer::Render() {	
	scene.get_active_camera().aspect = swapchain->GetAspect();
	sceneView->update(swapchain->GetFrameIndex());
	// pass 1 : cull & classify LOD
	// reset cmd buffer counter
	{	
		auto compute = device->GetCommandList<CommandListType::Compute>();
		compute->Begin();
		auto native = compute->GetNativeCommandList();
		native->SetPipelineState(*cullPassPSO);
		auto heap = device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap();
		native->SetDescriptorHeaps(1, &heap);
		native->SetComputeRootSignature(*cullPassRS);
		native->SetComputeRootConstantBufferView(0, sceneView->get_SceneGlobals()->GetGPUAddress());
		native->SetComputeRootShaderResourceView(1, sceneView->get_SceneMeshInstances()->GetGPUAddress());
		native->SetComputeRootDescriptorTable(2, indirectCmdBufferUAV->descriptor.get_gpu_handle());
		indirectCmdBuffer->SetBarrier(compute, ResourceState::CopyDest);
		ResetCounter(compute, indirectCmdBuffer.get(), CommandBufferCounterOffset);
		indirectCmdBuffer->SetBarrier(compute, ResourceState::UnorderedAccess);
		// dispatch compute to cull on the gpu
		native->Dispatch(DivRoundUp(sceneView->get_SceneStats().numMeshInstances, RENDERER_INSTANCE_CULL_THREADS), 1, 1);
		indirectCmdBuffer->SetBarrier(compute, ResourceState::IndirectArgument);
		compute->End();
		compute->Execute().Wait();		
	}
	RenderGraph rg(cache);
	{
		// pass 2 : gbuffer generation
		UINT width = swapchain->GetWidth(), height = swapchain->GetHeight();		
		// resources
		auto depthStencil = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
			ResourceFormat::D32_FLOAT, ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			ResourceFlags::DepthStencil, ResourceHeapType::Default,
			ResourceState::DepthWrite, 
#ifdef INVERSE_Z			
			ClearValue(0.0f, 0),
#else
			ClearValue(1.0f, 0),
#endif
			L"GBuffer Depth"
		));
		auto albedo = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
			ResourceFormat::R8G8B8A8_UNORM, ResourceDimension::Texture2D,
			width, height, 1, 1, 1, 0,
			ResourceFlags::RenderTarget, ResourceHeapType::Default,
			ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
			L"GBuffer Albedo"
		));
		auto dsv = rg.create<DepthStencilView>({
			.viewDesc = DepthStencilViewDesc::GetTexture2DDepthBufferDesc(ResourceFormat::D32_FLOAT, 0),
			.viewed = depthStencil
		});
		auto rtv = rg.create<RenderTargetView>({
			.viewDesc = RenderTargetViewDesc::GetTexture2DRenderTargetDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
			.viewed = albedo
		});
		rg.add_pass()
			.write(depthStencil)
			.write(albedo)
			.execute([=](RgContext& ctx) -> void {
				PIXBeginEvent(ctx.cmd->GetNativeCommandList(), 0, L"GBuffer");
				auto native = ctx.cmd->GetNativeCommandList();
				native->SetPipelineState(*gBufferPSO);
				native->SetGraphicsRootSignature(*gBufferRS);
				native->SetGraphicsRootConstantBufferView(0, sceneView->get_SceneGlobals()->GetGPUAddress());
				CD3DX12_VIEWPORT viewport(.0f, .0f, swapchain->GetWidth(), swapchain->GetHeight(), .0f, 1.0f);
				CD3DX12_RECT scissorRect(0, 0, swapchain->GetWidth(), swapchain->GetHeight());
				native->RSSetViewports(1, &viewport);
				native->RSSetScissorRects(1, &scissorRect);
				native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);				
				native->OMSetRenderTargets(
					1,
					&swapchain->GetBackbufferRTV(swapchain->GetCurrentBackbufferIndex()).descriptor.get_cpu_handle(), // xxx CHANGE THIS ASAP!!!!!
					/* &ctx.graph->get<RenderTargetView>(rtv)->descriptor.get_cpu_handle(), */
					FALSE,
					&ctx.graph->get<DepthStencilView>(dsv)->descriptor.get_cpu_handle()
				);
				const auto clearValue = ctx.graph->get<Texture>(depthStencil)->GetDesc().clearValue;
				native->ClearDepthStencilView(
					ctx.graph->get<DepthStencilView>(dsv)->descriptor.get_cpu_handle(),
					D3D12_CLEAR_FLAG_DEPTH,
					clearValue->depthStencil.depth, clearValue->depthStencil.stencil,
					1, &scissorRect
				);
				native->ExecuteIndirect(
					*gBufferIndirectCommandSig,
					MAX_INSTANCE_COUNT,
					*indirectCmdBuffer,
					0,
					*indirectCmdBuffer,
					CommandBufferCounterOffset
				);
				PIXEndEvent(ctx.cmd->GetNativeCommandList());
			});
		// pass 3 : lighting
		// ...tbd
		// epilouge : output
		rg.get_epilogue_pass().read(albedo);
	}
	rg.execute(device->GetCommandList<CommandListType::Direct>());
}