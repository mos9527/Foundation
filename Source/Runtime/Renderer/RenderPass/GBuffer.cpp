#include "GBuffer.hpp"
using namespace RHI;
GBufferPass::GBufferPass(Device* device) {
	// pass 1 : cull pass / lod / indirect cmd generation
	cullPassCS = std::make_unique<Shader>(L"Shaders/InstanceCull.hlsl", L"main", L"cs_6_6");
	cullPassRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.AddConstantBufferView(0, 0) // b0 space0 : SceneGlobals	
		.AddShaderResourceView(0, 0) // t0 space0 : SceneMeshInstance
		.AddUnorderedAccessViewWithCounter(0, 0)// u0 space0 : Indirect Commandlists
	);
	cullPassRS->SetName(L"Indirect Cull & LOD Classification");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *cullPassRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cullPassCS->GetData(), cullPassCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	cullPassPSO = std::make_unique<PipelineState>(device, std::move(pso));
	indirectCmdBuffer = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		CommandBufferSize, sizeof(IndirectCommand), ResourceState::CopyDest, ResourceHeapType::Default, ResourceFlags::UnorderedAccess
	));
	indirectCmdBuffer->SetName(L"Indirect Commands Buffer");
	indirectCmdBufferUAV = std::make_unique<UnorderedAccessView>(
		indirectCmdBuffer.get(), indirectCmdBuffer.get(), UnorderedAccessViewDesc::GetStructuredBufferDesc(
			0, MAX_INSTANCE_COUNT, sizeof(IndirectCommand), CommandBufferCounterOffset
		)
	);
	resetBuffer = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(sizeof(uint), sizeof(uint)));
	resetBuffer->SetName(L"Zero buffer");
	uint resetValue = 0;
	resetBuffer->Update(&resetValue, sizeof(resetValue), 0);
	// pass 2 : gbuffer generation
	gBufferPS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"ps_main", L"ps_6_6");
	gBufferVS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"vs_main", L"vs_6_6");
	gBufferRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetAllowInputAssembler()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 1) // b0 space0 : MeshIndex constant (Indirect)
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
	gbufferPsoDesc.NumRenderTargets = 4; // ALBEDO[RGB+Mask], NORMAL, MATERIAL[metallic,roughness,[packed uint16]], EMISSIVE
	gbufferPsoDesc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.RTVFormats[1] = ResourceFormatToD3DFormat(ResourceFormat::R16G16_FLOAT); // see Shared.h
	gbufferPsoDesc.RTVFormats[2] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.RTVFormats[3] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
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
}

void GBufferPass::insert(RenderGraph& rg, SceneGraphView* sceneView, GBufferPassHandles& handles) {
	auto localIntransients = rg.create<Dummy>({});
	rg.add_pass(L"Indirect Cull & LOD Indirect")
		.write(localIntransients)
		.execute([=](RgContext& ctx) -> void {
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetPipelineState(*cullPassPSO);
		native->SetComputeRootSignature(*cullPassRS);
		native->SetComputeRootConstantBufferView(0, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
		native->SetComputeRootShaderResourceView(1, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
		native->SetComputeRootDescriptorTable(2, indirectCmdBufferUAV->descriptor);
		indirectCmdBuffer->SetBarrier(ctx.cmd, ResourceState::CopyDest);
		native->CopyBufferRegion(*indirectCmdBuffer.get(), CommandBufferCounterOffset, *resetBuffer.get(), 0, sizeof(UINT)); // Resets UAV counter
		indirectCmdBuffer->SetBarrier(ctx.cmd, ResourceState::UnorderedAccess);
		// dispatch compute to cull on the gpu
		native->Dispatch(DivRoundUp(sceneView->get_SceneGlobals().numMeshInstances, RENDERER_INSTANCE_CULL_THREADS), 1, 1);
		indirectCmdBuffer->SetBarrier(ctx.cmd, ResourceState::IndirectArgument);
	});
	rg.add_pass(L"GBuffer Generation")
		.read(localIntransients)
		.write(handles.depth)
		.write(handles.albedo).write(handles.normal).write(handles.material).write(handles.emissive)
		.execute([=](RgContext& ctx) -> void {
			UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
			// fetches from registry
			auto* r_albedo_rtv = ctx.graph->get<RenderTargetView>(handles.albedo_rtv);
			auto* r_normal_rtv = ctx.graph->get<RenderTargetView>(handles.normal_rtv);
			auto* r_material_rtv = ctx.graph->get<RenderTargetView>(handles.material_rtv);
			auto* r_emissive_rtv = ctx.graph->get<RenderTargetView>(handles.emissive_rtv);
			auto* r_dsv = ctx.graph->get<DepthStencilView>(handles.depth_dsv);
			auto* r_depthStencil = ctx.graph->get<Texture>(handles.depth);
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*gBufferPSO);
			native->SetGraphicsRootSignature(*gBufferRS);
			// b0 used by indirect command
			native->SetGraphicsRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(2, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(3, sceneView->get_SceneMeshMaterialsBuffer()->GetGPUAddress());		
			CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, width, height);
			native->RSSetViewports(1, &viewport);
			native->RSSetScissorRects(1, &scissorRect);
			native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[4] = {
				r_albedo_rtv->descriptor,
				r_normal_rtv->descriptor,
				r_material_rtv->descriptor,
				r_emissive_rtv->descriptor
			};
			native->OMSetRenderTargets(
				4,
				rtvHandles,
				FALSE,
				&r_dsv->descriptor.get_cpu_handle()
			);
			native->ClearDepthStencilView(
				r_dsv->descriptor.get_cpu_handle(),
				D3D12_CLEAR_FLAG_DEPTH,
				r_depthStencil->GetClearValue().depthStencil.depth,
				r_depthStencil->GetClearValue().depthStencil.stencil,
				1, &scissorRect
			);
			const FLOAT clearColor[4] = { .0f ,.0f ,.0f ,.0f };
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
			native->ClearRenderTargetView(
				r_emissive_rtv->descriptor,
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
}