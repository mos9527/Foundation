#include "SilhouettePass.hpp"
using namespace RHI;
SilhouettePass::SilhouettePass(Device* device) {
	PS = BuildShader(L"SilhouettePass", L"ps_main", L"ps_6_6");
	VS = BuildShader(L"SilhouettePass", L"vs_main", L"vs_6_6");
	CS = BuildShader(L"SilhouettePassBlend", L"main", L"cs_6_6");
	RS = std::make_unique<RootSignature>( // Draw the selected meshes' depth in viewspace. Similar to shadow mapping
		device,
		RootSignatureDesc()
		.SetAllowInputAssembler()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 2) // b0 space0 : MeshIndex,LodIndex constant (Indirect)
		.AddConstantBufferView(1, 0) // b1 space0 : SceneGlobals		
		.AddShaderResourceView(0, 0) // t0 space0 : SceneMeshInstances		
	);
	RS->SetName(L"Silhouette");
	// Define the vertex input layout.
	auto iaLayout = VertexLayoutToD3DIADesc(StaticMeshAsset::Vertex::get_layout());
	// Describe and create the graphics pipeline state objects (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	psoDesc.pRootSignature = *RS;
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(VS->GetData(), VS->GetSize());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(PS->GetData(), PS->GetSize());
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);	
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#ifdef INVERSE_Z
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;	
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));	
	// Command for draws
	IndirectCmdSig = std::make_unique<CommandSignature>(
		device,
		RS.get(),
		CommandSignatureDesc(sizeof(IndirectCommand))
		.AddConstant(0, 0, 2) // b0 space0 : MeshIndex,LodIndex constant (Indirect)
		.AddVertexBufferView(0)
		.AddIndexBufferView()
		.AddDrawIndexed()
	);
	// Compute Edge detection + Blit to framebuffer
	blendRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 4) // Constants: width,height,framebuffer UAV,depth SRV
		.AddConstantBufferView(1,0) // b1 space0 : SceneGlobals		
	);
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *blendRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Blend = std::make_unique<PipelineState>(device, std::move(pso));
}
RenderGraphPass& SilhouettePass::insert(RenderGraph& rg, SceneView* sceneView, SilhouettePassHandles&& handles) {
	rg.add_pass(L"Silhouette Depth Rendering")
		.read(handles.silhouetteDepth)
		.write(handles.silhouetteDepth)
		.read(handles.silhouetteCMD)
		.execute([=](RgContext& ctx) {
			UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
			CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, width, height);			
			auto* r_dsv = ctx.graph->get<DepthStencilView>(handles.silhouetteDepthDSV);
			auto* r_cmd_uav = ctx.graph->get<UnorderedAccessView>(handles.silhouetteCMDUAV);
			auto* r_cmd = ctx.graph->get<Buffer>(handles.silhouetteCMD);
			auto native = ctx.cmd->GetNativeCommandList();
			ctx.cmd->QueueTransitionBarrier(r_cmd, ResourceState::IndirectArgument);
			ctx.cmd->FlushBarriers();
			native->SetPipelineState(*PSO);
			native->SetGraphicsRootSignature(*RS);
			native->SetGraphicsRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(2, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
			native->RSSetViewports(1, &viewport);
			native->RSSetScissorRects(1, &scissorRect);
			native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			native->OMSetRenderTargets(
				0,
				nullptr,
				FALSE,
				&r_dsv->descriptor.get_cpu_handle()
			);
			native->ExecuteIndirect(
				*IndirectCmdSig,
				MAX_INSTANCE_COUNT,
				r_cmd->GetNativeResource(),
				0,
				r_cmd->GetNativeResource(),
				r_cmd_uav->GetDesc().GetCounterOffsetInBytes()
			);
	});
	return rg.add_pass(L"Silhouette Blending")
		.read(handles.frameBuffer)
		.readwrite(handles.frameBuffer)
		.read(handles.silhouetteDepth)
		.execute([=](RgContext& ctx) {
			UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
			auto* r_depth_srv = ctx.graph->get<ShaderResourceView>(handles.silhouetteDepthSRV);
			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(handles.frameBufferUAV);			
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO_Blend);
			native->SetComputeRootSignature(*blendRS);
			native->SetComputeRoot32BitConstant(0, width, 0);
			native->SetComputeRoot32BitConstant(0, height, 1);
			native->SetComputeRoot32BitConstant(0, r_fb_uav->descriptor.get_heap_handle(), 2);
			native->SetComputeRoot32BitConstant(0, r_depth_srv->descriptor.get_heap_handle(), 3);
			native->SetComputeRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});
}