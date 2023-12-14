#include "Transparency.hpp"
using namespace RHI;
using namespace EditorGlobals;
void TransparencyPass::reset() {
	PS = BuildShader(L"Transparency", L"ps_main", L"ps_6_6");
	materialPS = BuildShader(L"Transparency", L"ps_main_material", L"ps_6_6");
	VS = BuildShader(L"Transparency", L"vs_main", L"vs_6_6");
	blendCS = BuildShader(L"TransparencyBlend", L"main", L"cs_6_6");
	// Define the vertex input layout.
	auto iaLayout = VertexLayoutToD3DIADesc(StaticMeshAsset::Vertex::get_layout());
	// Describe and create the graphics pipeline state objects (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	desc.pRootSignature = *g_RHI.rootSig;
	desc.VS = CD3DX12_SHADER_BYTECODE(VS->GetData(), VS->GetSize());
	desc.PS = CD3DX12_SHADER_BYTECODE(PS->GetData(), PS->GetSize());
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.BlendState.IndependentBlendEnable = TRUE;
	// Accumlation Buffer : Sum of (RGB*w A*w). RGBA16F
	desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
	desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
	// Revealage Buffer : Prod of (1 - A). R16
	desc.BlendState.RenderTarget[1].BlendEnable = TRUE;
	desc.BlendState.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
	desc.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
	desc.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR; // ( 1 - src.a ) * dest.a
	desc.BlendState.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ZERO;
	desc.BlendState.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; // ( 1 - src.a ) * dest.a; though we're not wrting to alpha channel here.
	// Disable depth writes. But still reads from GBuffer depth.
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	desc.DepthStencilState.DepthEnable = TRUE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;	
#ifdef INVERSE_Z
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 2;
	desc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R16G16B16A16_FLOAT);
	desc.RTVFormats[1] = ResourceFormatToD3DFormat(ResourceFormat::R16_FLOAT);
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);	
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // No culling	
	desc.RasterizerState.FillMode = g_Editor.render.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	// Material PSO
	// Needed since the transparency pass alone does not write to Depth
	// nor Material. Needed for object picking.
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // Disable blending
	desc.PS = CD3DX12_SHADER_BYTECODE(materialPS->GetData(), materialPS->GetSize());
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; // Write to depth & test
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM); // -> Material buffer
	desc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
	PSO_Material = std::make_unique<PipelineState>(device, std::move(pso));
	// Command for draws
	IndirectCmdSig = std::make_unique<CommandSignature>(
		device,
		g_RHI.rootSig,
		CommandSignatureDesc(sizeof(IndirectCommand))
		.AddConstant(0, 0, 2) // b0 space0 : MeshIndex,LodIndex constant (Indirect)
		.AddVertexBufferView(0)
		.AddIndexBufferView()
		.AddDrawIndexed()
	);
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *g_RHI.rootSig;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(blendCS->GetData(), blendCS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Blend = std::make_unique<PipelineState>(device, std::move(pso));
}

// Transparency objects don't occlude each other
// so no early/late draws. One is enough.
// The commands comes from the late cull, which only excludes
// transparent objects that are occluded by opaque ones
// or that failed the frustum test.
RenderGraphPass& TransparencyPass::insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles) {	
	auto setup_pso = [=](RgContext& ctx) {
		UINT width = g_Editor.render.width, height = g_Editor.render.height;
		CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
		CD3DX12_RECT scissorRect(0, 0, width, height);
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetGraphicsRootSignature(*g_RHI.rootSig);
		native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->GetGlobalsBuffer().GetGPUAddress());
		native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, sceneView->GetShadingBuffer().GetGPUAddress());
		native->RSSetViewports(1, &viewport);
		native->RSSetScissorRects(1, &scissorRect);
		native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);		
	};
	auto setup_wboit = [=](RgContext& ctx) {
		UINT width = g_Editor.render.width, height = g_Editor.render.height;
		CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
		CD3DX12_RECT scissorRect(0, 0, width, height);

		auto* r_depth = ctx.graph->get<Texture>(*handles.depth_dsv.first);
		auto* r_dsv = ctx.graph->get<DepthStencilView>(*handles.depth_dsv.second);
		auto* r_acc = ctx.graph->get<Texture>(*std::get<0>(handles.accumaltion_rtv_srv));
		auto* r_rev = ctx.graph->get<Texture>(*std::get<0>(handles.revealage_rtv_srv));
		auto* r_acc_rtv = ctx.graph->get<RenderTargetView>(*std::get<1>(handles.accumaltion_rtv_srv));
		auto* r_rev_rtv = ctx.graph->get<RenderTargetView>(*std::get<1>(handles.revealage_rtv_srv));

		auto native = ctx.cmd->GetNativeCommandList();
		ctx.cmd->QueueTransitionBarrier(r_depth, ResourceState::DepthRead);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[2] = {
			r_acc_rtv->get_descriptor().get_cpu_handle(),
			r_rev_rtv->get_descriptor().get_cpu_handle()
		};
		native->OMSetRenderTargets(
			2,
			rtvHandles,
			FALSE,
			&r_dsv->get_descriptor().get_cpu_handle()
		);
	};
	auto setup_material = [=](RgContext& ctx) {
		UINT width = g_Editor.render.width, height = g_Editor.render.height;
		CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
		CD3DX12_RECT scissorRect(0, 0, width, height);

		auto* r_depth = ctx.graph->get<Texture>(*handles.depth_dsv.first);
		auto* r_dsv = ctx.graph->get<DepthStencilView>(*handles.depth_dsv.second);
		auto* r_material_rtv = ctx.graph->get<RenderTargetView>(*handles.material_srv.second);
		auto native = ctx.cmd->GetNativeCommandList();
		ctx.cmd->QueueTransitionBarrier(r_depth, ResourceState::DepthWrite);
		native->OMSetRenderTargets(
			1,
			&r_material_rtv->get_descriptor().get_cpu_handle(),
			FALSE,
			&r_dsv->get_descriptor().get_cpu_handle()
		);
	};
	auto render = [=](RgContext& ctx) {
		auto* r_indirect_commands = ctx.graph->get<Buffer>(*handles.command_uav.first);
		auto* r_indirect_commands_uav = ctx.graph->get<UnorderedAccessView>(*handles.command_uav.second);
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
		.read(*std::get<0>(handles.accumaltion_rtv_srv))
		.read(*std::get<0>(handles.revealage_rtv_srv))
		.write(*std::get<0>(handles.accumaltion_rtv_srv))
		.write(*std::get<0>(handles.revealage_rtv_srv))
		.read(*handles.depth_dsv.first)
		.read(*handles.command_uav.first)
		.execute([=](RgContext& ctx) {
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetPipelineState(*PSO);
		setup_pso(ctx);		
		setup_wboit(ctx);
		render(ctx);
	});	
	rg.add_pass(L"Transparency Material Buffer Append")
		.read(*handles.material_srv.first)
		.write(*handles.material_srv.first)
		.write(*handles.depth_dsv.first)
		.read(*handles.command_uav.first)
		.execute([=](RgContext& ctx) {
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetPipelineState(*PSO_Material);
		setup_pso(ctx);
		setup_material(ctx);
		render(ctx);
	});
	return rg.add_pass(L"Transparency Blending")	
		.read(*handles.framebuffer_uav.first)
		.readwrite(*handles.framebuffer_uav.first)
		.read(*std::get<0>(handles.accumaltion_rtv_srv))
		.read(*std::get<0>(handles.revealage_rtv_srv))
		.execute([=](RgContext& ctx) {
			UINT width = g_Editor.render.width, height = g_Editor.render.height;
			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(*handles.framebuffer_uav.second);
			auto* r_acc_srv = ctx.graph->get<ShaderResourceView>(*std::get<2>(handles.accumaltion_rtv_srv));
			auto * r_rev_srv = ctx.graph->get<ShaderResourceView>(*std::get<2>(handles.revealage_rtv_srv));
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO_Blend);
			native->SetComputeRootSignature(*g_RHI.rootSig);
			UINT constants[5] = {
				r_fb_uav->allocate_online_descriptor().get_heap_handle(),
				r_acc_srv->allocate_online_descriptor().get_heap_handle(),
				r_rev_srv->allocate_online_descriptor().get_heap_handle(),
				width,
				height
			};
			native->SetComputeRoot32BitConstants(RHIContext::ROOTSIG_CB_8_CONSTANTS, 5, constants, 0);
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});
}
