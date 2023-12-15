#include "SilhouettePass.hpp"
using namespace RHI;
using namespace EditorGlobals;

void SilhouettePass::reset() {
	PS = build_shader(0, L"ps_main", L"ps_6_6");
	VS = build_shader(0, L"vs_main", L"vs_6_6");
	CS = build_shader(1, L"main", L"cs_6_6");	
	auto iaLayout = VertexLayoutToD3DIADesc(StaticMeshAsset::Vertex::get_layout());
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	desc.pRootSignature = *g_RHI.rootSig;
	desc.VS = CD3DX12_SHADER_BYTECODE(VS->GetData(), VS->GetSize());
	desc.PS = CD3DX12_SHADER_BYTECODE(PS->GetData(), PS->GetSize());
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);	
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	desc.DepthStencilState.DepthEnable = TRUE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#ifdef INVERSE_Z
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 0;	
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));	
	// Command for draws
	IndirectCmdSig = std::make_unique<CommandSignature>(
		device,
		g_RHI.rootSig,
		CommandSignatureDesc(sizeof(IndirectCommand))
		.AddConstant(2, 0, 2) // b2 space0 : Mesh Index,LOD Index
		.AddVertexBufferView(0)
		.AddIndexBufferView()
		.AddDrawIndexed()
	);
	D3D12_COMPUTE_PIPELINE_STATE_DESC computedesc = {};
	computedesc.pRootSignature = *g_RHI.rootSig;
	computedesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computedesc, IID_PPV_ARGS(&pso)));
	PSO_Blend = std::make_unique<PipelineState>(device, std::move(pso));
	constants = std::make_unique<BufferContainer<SilhouetteConstants>>(device, 1, L"Silhouette Constants");
}
RenderGraphPass& SilhouettePass::insert(RenderGraph& rg, SceneView* sceneView, SilhouettePassHandles const& handles) {
	rg.add_pass(L"Silhouette Depth Rendering")
		.read(*std::get<0>(handles.silhouette_dsv_srv)) // Ensure cleared
		.write(*std::get<0>(handles.silhouette_dsv_srv))
		.read(*handles.command_uav.first)
		.execute([=](RgContext& ctx) {
			UINT width = g_Editor.render.width, height = g_Editor.render.height;
			auto* native = ctx.cmd->GetNativeCommandList();

			CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, width, height);			
			auto* r_dsv = ctx.graph->get<DepthStencilView>(*std::get<1>(handles.silhouette_dsv_srv));			
			
			auto* r_cmd = ctx.graph->get<Buffer>(*handles.command_uav.first);
			auto* r_cmd_uav = ctx.graph->get<UnorderedAccessView>(*handles.command_uav.second);

			ctx.cmd->QueueTransitionBarrier(r_cmd, ResourceState::IndirectArgument);
			ctx.cmd->FlushBarriers();

			constants->Data()->edgeColor = EditorGlobals::g_Editor.pickerSilhouette.edgeColor;
			constants->Data()->edgeThreshold = EditorGlobals::g_Editor.pickerSilhouette.edgeThreshold;

			native->SetPipelineState(*PSO);
			native->SetGraphicsRootSignature(*g_RHI.rootSig);
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->GetGlobalsBuffer().GetGPUAddress());
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->RSSetViewports(1, &viewport);
			native->RSSetScissorRects(1, &scissorRect);
			native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			native->OMSetRenderTargets(
				0,
				nullptr,
				FALSE,
				&r_dsv->get_descriptor().get_cpu_handle()
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
		.read(*handles.framebuffer_uav.first)
		.readwrite(*handles.framebuffer_uav.first)
		.read(*std::get<0>(handles.silhouette_dsv_srv))
		.execute([=](RgContext& ctx) {
			UINT width = g_Editor.render.width, height = g_Editor.render.height;
			auto* native = ctx.cmd->GetNativeCommandList();

			auto* r_depth_srv = ctx.graph->get<ShaderResourceView>(*std::get<2>(handles.silhouette_dsv_srv));
			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(*handles.framebuffer_uav.second);
			
			constants->Data()->frameBufferUAV = r_fb_uav->allocate_online_descriptor().get_heap_handle();
			constants->Data()->depthSRV = r_depth_srv->allocate_online_descriptor().get_heap_handle();

			native->SetPipelineState(*PSO_Blend);
			native->SetGraphicsRootSignature(*g_RHI.rootSig);
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->GetGlobalsBuffer().GetGPUAddress());
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});
}
