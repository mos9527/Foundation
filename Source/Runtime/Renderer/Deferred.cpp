#include "Deferred.hpp"
#include "../AssetRegistry/IO.hpp"
using namespace RHI;
void DeferredRenderer::ResetCounter(CommandList* cmd, Resource* resource, size_t offset) {
	cmd->CopyBufferRegion(resetBuffer.get(), resource, 0, offset, sizeof(UINT));
}
DeferredRenderer::DeferredRenderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapchain) : Renderer(assets, scene, device, swapchain) {
	sceneView = std::make_unique<SceneGraphView>(device, scene);
	// pass 1 : cull pass
	cullPassCS = std::make_unique<Shader>(L"Shaders/InstanceCull.hlsl", L"main", L"cs_6_6");
	cullPassRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()			
			.AddConstantBufferView(0,0) // b0 space0 : SceneGlobals	
			.AddShaderResourceView(0,0) // t0 space0 : SceneMeshInstance
			.AddUnorderedAccessViewWithCounter(0,0)// u0 space0 : Indirect Commandlists
	);
	cullPassRS->SetName(L"Indirect Cull & LOD Classification");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *cullPassRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cullPassCS->GetData(), cullPassCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	cullPassPSO = std::make_unique<PipelineState>(device, std::move(pso));

	// pass 2 : gbuffer pass
	gBufferPS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"ps_main", L"ps_6_6");
	gBufferVS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"vs_main", L"vs_6_6");
	gBufferRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetAllowInputAssembler()
		.SetDirectlyIndexed()
		.AddConstant(0,0,1) // b0 space0 : MeshIndex constant (Indirect)
		.AddConstantBufferView(1, 0) // b1 space0 : SceneGlobals		
		.AddShaderResourceView(0, 0) // t0 space0 : SceneMeshInstances
		.AddShaderResourceView(1, 0) // t1 space0 : SceneMaterials
		.AddStaticSampler(0, 0) // s0 space0 : Texture Sampler
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
	gbufferPsoDesc.NumRenderTargets = 3; // ALBEDO[RGB+Mask], NORMAL, MATERIAL[metallic,roughness,[packed uint16]]
	gbufferPsoDesc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.RTVFormats[1] = ResourceFormatToD3DFormat(ResourceFormat::R16G16_FLOAT); // see Shared.h
	gbufferPsoDesc.RTVFormats[2] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	gbufferPsoDesc.SampleDesc.Count = 1;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&pso)));
	gBufferPSO = std::make_unique<PipelineState>(device, std::move(pso));
	// indirect command buffer
	gBufferIndirectCommandSig = std::make_unique<CommandSignature>(
		device,
		gBufferRS.get(),
		CommandSignatureDesc(sizeof(IndirectCommand))
			.AddConstant(0, 0, 1)
			.AddVertexBufferView(0)
			.AddIndexBufferView()
			.AddDrawIndexed()			
	);

	// pass 3 : lighting
	lightingCS = std::make_unique<Shader>(L"Shaders/Lighting.hlsl", L"main", L"cs_6_6");
	lightingRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : SceneGlobals	
		.AddConstant(1, 0, 5) // b1 space0 : MRT handles + FB UAV
		.AddUnorderedAccessViewWithCounter(0, 0)// u0 space0 : Framebuffer (RWTexture2D)
	);
	lightingRS->SetName(L"Lighting");
	computePsoDesc = {};
	computePsoDesc.pRootSignature = *lightingRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(lightingCS->GetData(), lightingCS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	lightingPSO = std::make_unique<PipelineState>(device, std::move(pso));

	// non-transient resources
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
ShaderResourceView* DeferredRenderer::Render() {	
	scene.get_active_camera().aspect = swapchain->GetAspect();
	UINT width = swapchain->GetWidth(), height = swapchain->GetHeight();
	UINT frame = swapchain->GetFrameIndex();
	sceneView->update(width, height, frame);
	// pass 1 : cull & classify LOD (intransient)
	// reset cmd buffer counter
	RenderGraph rg(cache);
	auto intransients = rg.create<Dummy>({});
	rg.add_pass(L"Culling & LOD")
		.write(intransients)
		.execute([=](RgContext& ctx) -> void {			
			auto native = ctx.cmd->GetNativeCommandList();			
			native->SetPipelineState(*cullPassPSO);
			native->SetComputeRootSignature(*cullPassRS);
			native->SetComputeRootConstantBufferView(0, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->SetComputeRootShaderResourceView(1, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
			native->SetComputeRootDescriptorTable(2, indirectCmdBufferUAV->descriptor);
			indirectCmdBuffer->SetBarrier(ctx.cmd, ResourceState::CopyDest);
			ResetCounter(ctx.cmd, indirectCmdBuffer.get(), CommandBufferCounterOffset);
			indirectCmdBuffer->SetBarrier(ctx.cmd, ResourceState::UnorderedAccess);
			// dispatch compute to cull on the gpu
			native->Dispatch(DivRoundUp(sceneView->get_SceneGlobals().numMeshInstances, RENDERER_INSTANCE_CULL_THREADS), 1, 1);
			indirectCmdBuffer->SetBarrier(ctx.cmd, ResourceState::IndirectArgument);		
	});
	// pass 2 : gbuffer generation
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
	auto normal = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16_FLOAT, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
		L"Normal Buffer"
	));
	auto material = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R8G8B8A8_UNORM, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::RenderTarget, ResourceHeapType::Default,
		ResourceState::RenderTarget, ClearValue(0, 0, 0, 0),
		L"Material Buffer"
	));
	auto dsv = rg.create<DepthStencilView>({
		.viewDesc = DepthStencilViewDesc::GetTexture2DDepthBufferDesc(ResourceFormat::D32_FLOAT, 0),
		.viewed = depthStencil
	});
	auto albedo_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DRenderTargetDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
		.viewed = albedo
	});
	auto normal_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DRenderTargetDesc(ResourceFormat::R16G16_FLOAT, 0),
		.viewed = normal
	});
	auto material_rtv = rg.create<RenderTargetView>({
		.viewDesc = RenderTargetViewDesc::GetTexture2DRenderTargetDesc(ResourceFormat::R8G8B8A8_UNORM, 0),
		.viewed = material
	});
	rg.add_pass(L"GBuffer Generation")
		.read(intransients)
		.write(depthStencil)
		.write(albedo).write(normal).write(material)
		.execute([=](RgContext& ctx) -> void {			
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*gBufferPSO);
			native->SetGraphicsRootSignature(*gBufferRS);
			// b0 used by indirect command
			native->SetGraphicsRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(2, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(3, sceneView->get_SceneMeshMaterialsBuffer()->GetGPUAddress());
			CD3DX12_VIEWPORT viewport(.0f, .0f, swapchain->GetWidth(), swapchain->GetHeight(), .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, swapchain->GetWidth(), swapchain->GetHeight());
			native->RSSetViewports(1, &viewport);
			native->RSSetScissorRects(1, &scissorRect);
			native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			// fetches from registry
			auto* r_albedo_rtv = ctx.graph->get<RenderTargetView>(albedo_rtv);
			auto* r_normal_rtv = ctx.graph->get<RenderTargetView>(normal_rtv);
			auto* r_material_rtv = ctx.graph->get<RenderTargetView>(material_rtv);
			auto* r_dsv = ctx.graph->get<DepthStencilView>(dsv);		
			auto* r_depthStencil = ctx.graph->get<Texture>(depthStencil);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[3] = {
				r_albedo_rtv->descriptor,
				r_normal_rtv->descriptor,
				r_material_rtv->descriptor,
			};
			native->OMSetRenderTargets(
				3,
				rtvHandles,					
				FALSE,
				&r_dsv->descriptor.get_cpu_handle()
			);
			native->ClearDepthStencilView(
				ctx.graph->get<DepthStencilView>(dsv)->descriptor.get_cpu_handle(),
				D3D12_CLEAR_FLAG_DEPTH,
				r_depthStencil->GetClearValue().depthStencil.depth,
				r_depthStencil->GetClearValue().depthStencil.stencil,
				1, &scissorRect
			);
			const FLOAT clearColor[4] = {.0f ,.0f ,.0f ,.0f};
			native->ClearRenderTargetView(
				r_albedo_rtv->descriptor,
				clearColor,
				1, &scissorRect
			);
			native->ClearRenderTargetView(
				r_normal_rtv->descriptor,
				clearColor,
				1, &scissorRect
			);
			native->ClearRenderTargetView(
				r_material_rtv->descriptor,
				clearColor,
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
		});
	// pass 3 : lighting
	auto frameBuffer = rg.create<Texture>(Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R8G8B8A8_UNORM, ResourceDimension::Texture2D,
		width, height, 1, 1, 1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess, {}, 
		L"Frame Buffer"
	));
	auto albedo_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0, 1),
		.viewed = albedo
	});
	auto normal_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R16G16_FLOAT, 0, 1),
		.viewed = normal
	});
	auto material_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0, 1),
		.viewed = material
	});
	auto depth_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R32_FLOAT, 0, 1),
		.viewed = depthStencil
	});
	auto fb_uav = rg.create<UnorderedAccessView>({
		.viewDesc = UnorderedAccessViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM,0,0),
		.viewed = frameBuffer
	});
	auto fb_srv = rg.create<ShaderResourceView>({
		.viewDesc = ShaderResourceViewDesc::GetTexture2DDesc(ResourceFormat::R8G8B8A8_UNORM, 0, 1),
		.viewed = frameBuffer
	});
	rg.add_pass(L"Lighting")
		.readwrite(frameBuffer)
		.read(depthStencil).read(albedo).read(normal).read(material)
		.execute([=](RgContext& ctx) {
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*lightingPSO);;
			native->SetComputeRootSignature(*lightingRS);
			native->SetComputeRootConstantBufferView(0, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			auto* r_albedo_srv = ctx.graph->get<ShaderResourceView>(albedo_srv);
			auto* r_normal_srv = ctx.graph->get<ShaderResourceView>(normal_srv);
			auto* r_material_srv = ctx.graph->get<ShaderResourceView>(material_srv);
			auto* r_depth_srv = ctx.graph->get<ShaderResourceView>(depth_srv);
			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(fb_uav);
			UINT mrtHandles[5] = { 
				r_albedo_srv->descriptor.get_heap_handle(),
				r_normal_srv->descriptor.get_heap_handle(),
				r_material_srv->descriptor.get_heap_handle(),
				r_depth_srv->descriptor.get_heap_handle(),
				r_fb_uav->descriptor.get_heap_handle()
			};
			native->SetComputeRoot32BitConstants(1, 5, mrtHandles, 0);
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);				
	});
	// epilouge : output
	rg.get_epilogue_pass().read(frameBuffer);
	rg.execute(device->GetCommandList<CommandListType::Direct>());
	return rg.get<ShaderResourceView>(albedo_srv);
}