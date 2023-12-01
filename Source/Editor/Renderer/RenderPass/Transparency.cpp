#include "Transparency.hpp"
using namespace RHI;
TransparencyPass::TransparencyPass(Device* device) {
	PS = BuildShader(L"Transparency", L"ps_main", L"ps_6_6");
	materialPS = BuildShader(L"Transparency", L"ps_main_material", L"ps_6_6");
	VS = BuildShader(L"Transparency", L"vs_main", L"vs_6_6");
	blendCS = BuildShader(L"TransparencyBlend", L"main", L"cs_6_6");
	RS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetAllowInputAssembler()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 2) // b0 space0 : MeshIndex,LodIndex constant (Indirect)
		.AddConstantBufferView(1, 0) // b1 space0 : SceneGlobals		
		.AddShaderResourceView(0, 0) // t0 space0 : SceneMeshInstances
		.AddShaderResourceView(1, 0) // t1 space0 : SceneMaterials
		.AddShaderResourceView(2, 0) // t2 space0 : SceneLights
		.AddStaticSampler(0, 0) // s0 space0 : Texture Sampler
	);
	RS->SetName(L"Transparency Shading");
	// Define the vertex input layout.
	auto iaLayout = VertexLayoutToD3DIADesc(MeshAsset::Vertex::get_layout());
	// Describe and create the graphics pipeline state objects (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC transparencyPsoDesc = {};
	transparencyPsoDesc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	transparencyPsoDesc.pRootSignature = *RS;
	transparencyPsoDesc.VS = CD3DX12_SHADER_BYTECODE(VS->GetData(), VS->GetSize());
	transparencyPsoDesc.PS = CD3DX12_SHADER_BYTECODE(PS->GetData(), PS->GetSize());
	transparencyPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);	
	transparencyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // No culling
	transparencyPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	transparencyPsoDesc.BlendState.IndependentBlendEnable = TRUE;
	// Accumlation Buffer : Sum of (RGB*w A*w). RGBA16F
	transparencyPsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	transparencyPsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	transparencyPsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
	transparencyPsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	transparencyPsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	transparencyPsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
	// Revealage Buffer : Prod of (1 - A). R16
	transparencyPsoDesc.BlendState.RenderTarget[1].BlendEnable = TRUE;
	transparencyPsoDesc.BlendState.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
	transparencyPsoDesc.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
	transparencyPsoDesc.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR; // ( 1 - src.a ) * dest.a
	transparencyPsoDesc.BlendState.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ZERO;
	transparencyPsoDesc.BlendState.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; // ( 1 - src.a ) * dest.a; though we're not wrting to alpha channel here.
	// Disable depth writes. But still reads from GBuffer depth.
	transparencyPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	transparencyPsoDesc.DepthStencilState.DepthEnable = TRUE;
	transparencyPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;	
#ifdef INVERSE_Z
	transparencyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
	transparencyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	transparencyPsoDesc.SampleMask = UINT_MAX;
	transparencyPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	transparencyPsoDesc.NumRenderTargets = 2;
	transparencyPsoDesc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R16G16B16A16_FLOAT);
	transparencyPsoDesc.RTVFormats[1] = ResourceFormatToD3DFormat(ResourceFormat::R16_FLOAT);
	transparencyPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	transparencyPsoDesc.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&transparencyPsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	// Wireframe PSO
	transparencyPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&transparencyPsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Wireframe = std::make_unique<PipelineState>(device, std::move(pso));
	// Material PSO
	// Needed since the transparency pass alone does not write to Depth
	// nor Material. Needed for object picking.
	transparencyPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // Disable blending
	transparencyPsoDesc.PS = CD3DX12_SHADER_BYTECODE(materialPS->GetData(), materialPS->GetSize());
	transparencyPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; // Write to depth & test
	transparencyPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	transparencyPsoDesc.NumRenderTargets = 1;
	transparencyPsoDesc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM); // -> Material buffer
	transparencyPsoDesc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&transparencyPsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Material = std::make_unique<PipelineState>(device, std::move(pso));
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
	// Compute blend
	blendRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstant(0,0,5) // Constants: Framebuffer UAV, Acc buffer SRV, Rev buffer SRV ,width,height
	);
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *blendRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(blendCS->GetData(), blendCS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Blend = std::make_unique<PipelineState>(device, std::move(pso));
}

// Transparency objects don't occlude each other
// so no early/late draws. One is enough.
// The commands comes from the late cull, which only excludes
// transparent objects that are occluded by opaque ones
// or that failed the frustum test.
RenderGraphPass& TransparencyPass::insert(RenderGraph& rg, SceneView* sceneView, TransparencyPassHandles&& handles) {	
	auto setup_pso = [=](RgContext& ctx) {
		UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
		CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
		CD3DX12_RECT scissorRect(0, 0, width, height);
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetGraphicsRootSignature(*RS);		
		native->SetGraphicsRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
		native->SetGraphicsRootShaderResourceView(2, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
		native->SetGraphicsRootShaderResourceView(3, sceneView->get_SceneMeshMaterialsBuffer()->GetGPUAddress());
		native->SetGraphicsRootShaderResourceView(4, sceneView->get_SceneLightBuffer()->GetGPUAddress());
		native->RSSetViewports(1, &viewport);
		native->RSSetScissorRects(1, &scissorRect);
		native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);		
	};
	auto setup_wboit = [=](RgContext& ctx) {
		UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
		CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
		CD3DX12_RECT scissorRect(0, 0, width, height);

		auto* r_depth = ctx.graph->get<Texture>(handles.depth);
		auto* r_dsv = ctx.graph->get<DepthStencilView>(handles.depth_dsv);
		auto* r_acc = ctx.graph->get<Texture>(handles.accumalationBuffer);
		auto* r_rev = ctx.graph->get<Texture>(handles.revealageBuffer);
		auto* r_acc_rtv = ctx.graph->get<RenderTargetView>(handles.accumalationBuffer_rtv);
		auto* r_rev_rtv = ctx.graph->get<RenderTargetView>(handles.revealageBuffer_rtv);

		auto native = ctx.cmd->GetNativeCommandList();
		native->ClearRenderTargetView(r_acc_rtv->descriptor.get_cpu_handle(), r_acc->GetClearValue().color, 1, &scissorRect);
		native->ClearRenderTargetView(r_rev_rtv->descriptor.get_cpu_handle(), r_rev->GetClearValue().color, 1, &scissorRect);		
		ctx.cmd->QueueTransitionBarrier(r_depth, ResourceState::DepthRead);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2] = {
			r_acc_rtv->descriptor.get_cpu_handle(),
			r_rev_rtv->descriptor.get_cpu_handle()
		};
		native->OMSetRenderTargets(
			2,
			rtvHandles,
			FALSE,
			&r_dsv->descriptor.get_cpu_handle()
		);
	};
	auto setup_material = [=](RgContext& ctx) {
		UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
		CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
		CD3DX12_RECT scissorRect(0, 0, width, height);

		auto* r_depth = ctx.graph->get<Texture>(handles.depth);
		auto* r_dsv = ctx.graph->get<DepthStencilView>(handles.depth_dsv);
		auto* r_material_rtv = ctx.graph->get<RenderTargetView>(handles.material_rtv);
		auto native = ctx.cmd->GetNativeCommandList();
		ctx.cmd->QueueTransitionBarrier(r_depth, ResourceState::DepthWrite);
		native->OMSetRenderTargets(
			1,
			&r_material_rtv->descriptor.get_cpu_handle(),
			FALSE,
			&r_dsv->descriptor.get_cpu_handle()
		);
	};
	auto render = [=](RgContext& ctx) {
		auto* r_indirect_commands = ctx.graph->get<Buffer>(handles.transparencyIndirectCommands);
		auto* r_indirect_commands_uav = ctx.graph->get<UnorderedAccessView>(handles.transparencyIndirectCommandsUAV);
		ctx.cmd->QueueTransitionBarrier(r_indirect_commands, ResourceState::IndirectArgument);
		ctx.cmd->FlushBarriers();
		auto native = ctx.cmd->GetNativeCommandList();
		native->ExecuteIndirect(
			*IndirectCmdSig,
			MAX_INSTANCE_COUNT,
			r_indirect_commands->GetNativeResource(),
			0,
			r_indirect_commands->GetNativeResource(),
			r_indirect_commands_uav->GetDesc().GetCounterOffsetInBytes()
		);
	};
	rg.add_pass(L"Transparency Rendering")
		.write(handles.accumalationBuffer)
		.write(handles.revealageBuffer)		
		.read(handles.depth)
		.read(handles.transparencyIndirectCommands)
		.execute([=](RgContext& ctx) {
		bool wireframe = sceneView->get_SceneGlobals().frameFlags & FRAME_FLAG_WIREFRAME;
		auto native = ctx.cmd->GetNativeCommandList();
		if (wireframe) {
			native->SetPipelineState(*PSO_Wireframe);
			setup_pso(ctx);
		}
		else {
			native->SetPipelineState(*PSO);
			setup_pso(ctx);
		}
		setup_wboit(ctx);
		render(ctx);
	});	
	rg.add_pass(L"Transparency Material Buffer Append")
		.read(handles.material)
		.write(handles.material)
		.write(handles.depth)
		.read(handles.transparencyIndirectCommands)
		.execute([=](RgContext& ctx) {
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetPipelineState(*PSO_Material);
		setup_pso(ctx);
		setup_material(ctx);
		render(ctx);
	});
	return rg.add_pass(L"Transparency Blending")	
		.read(handles.framebuffer)
		.readwrite(handles.framebuffer)
		.read(handles.accumalationBuffer)
		.read(handles.revealageBuffer)
		.execute([=](RgContext& ctx) {
			UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(handles.fb_uav);
			auto* r_acc_srv = ctx.graph->get<ShaderResourceView>(handles.accumalationBuffer_srv);
			auto* r_rev_srv = ctx.graph->get<ShaderResourceView>(handles.revealageBuffer_srv);
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO_Blend);
			native->SetComputeRootSignature(*blendRS);
			UINT constants[5] = {
				r_fb_uav->descriptor.get_heap_handle(),
				r_acc_srv->descriptor.get_heap_handle(),
				r_rev_srv->descriptor.get_heap_handle(),
				width,
				height
			};
			native->SetComputeRoot32BitConstants(0, 5, constants, 0);
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});
}