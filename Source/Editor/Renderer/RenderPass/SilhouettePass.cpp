#include "SilhouettePass.hpp"
using namespace RHI;
using namespace EditorGlobals;

void SilhouettePass::setup() {
	PS = BuildShader(L"SilhouettePass", L"ps_main", L"ps_6_6");
	VS = BuildShader(L"SilhouettePass", L"vs_main", L"vs_6_6");
	CS = BuildShader(L"SilhouettePassBlend", L"main", L"cs_6_6");	
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
		.AddConstant(0, 0, 2) // b0 space0 : MeshIndex,LodIndex constant (Indirect)
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
RenderGraphPass& SilhouettePass::insert(RenderGraph& rg, SceneView* sceneView, SilhouettePassHandles&& handles) {
	rg.add_pass(L"Silhouette Depth Rendering")
		.read(*std::get<0>(handles.silhouette_dsv_srv)) // Ensure cleared
		.write(*std::get<0>(handles.silhouette_dsv_srv))
		.read(*handles.cmd_uav.first)
		.execute([=](RgContext& ctx) {
			UINT width = g_Editor.render.width, height = g_Editor.render.height;
			auto* native = ctx.cmd->GetNativeCommandList();

			CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, width, height);			
			auto* r_dsv = ctx.graph->get<DepthStencilView>(*std::get<1>(handles.silhouette_dsv_srv));			
			
			auto* r_cmd = ctx.graph->get<Buffer>(*handles.cmd_uav.first);
			auto* r_cmd_uav = ctx.graph->get<UnorderedAccessView>(*handles.cmd_uav.second);

			ctx.cmd->QueueTransitionBarrier(r_cmd, ResourceState::IndirectArgument);
			ctx.cmd->FlushBarriers();

			constants->Data()->edgeColor = sceneView->get_shader_data().silhouette.edgeColor;
			constants->Data()->edgeThreshold = sceneView->get_shader_data().silhouette.edgeThreshold;

			native->SetPipelineState(*PSO);
			native->SetGraphicsRootSignature(*g_RHI.rootSig);
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->get_editor_globals_buffer()->GetGPUAddress());
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
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
		.read(*handles.frameBuffer_uav.first)
		.readwrite(*handles.frameBuffer_uav.first)
		.read(*std::get<0>(handles.silhouette_dsv_srv))
		.execute([=](RgContext& ctx) {
			UINT width = g_Editor.render.width, height = g_Editor.render.height;
			auto* native = ctx.cmd->GetNativeCommandList();

			auto* r_depth_srv = ctx.graph->get<ShaderResourceView>(*std::get<2>(handles.silhouette_dsv_srv));
			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(*handles.frameBuffer_uav.second);
			
			constants->Data()->frameBufferUAV = r_fb_uav->descriptor.get_heap_handle();
			constants->Data()->depthSRV = r_depth_srv->descriptor.get_heap_handle();

			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO_Blend);
			native->SetGraphicsRootSignature(*g_RHI.rootSig);
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->get_editor_globals_buffer()->GetGPUAddress());
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});
}
