#include "DeferredLighting.hpp"
using namespace RHI;
using namespace EditorGlobals;
void DeferredLightingPass::reset() {
	CS = build_shader(0, L"main", L"cs_6_6");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *g_RHI.rootSig;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
}

RenderGraphPass& DeferredLightingPass::insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles) {
	return rg.add_pass(L"Lighting")
		.readwrite(*handles.framebuffer_uav.first)
		.read(*handles.depth_srv.first)
		.read(*handles.tangentframe_srv.first)
		.read(*handles.material_srv.first)
		.read(*handles.gradient_srv.first)		
		.execute([=](RgContext& ctx) {
			UINT width = g_Editor.render.width, height = g_Editor.render.height;			
			auto* native = ctx.cmd->GetNativeCommandList();
			const DeferredShadingConstants constants{
				.tangentFrameSrv = ctx.graph->get<ShaderResourceView>(*handles.tangentframe_srv.second)->allocate_online_descriptor().get_heap_handle(),
				.gradientSrv = ctx.graph->get<ShaderResourceView>(*handles.gradient_srv.second)->allocate_online_descriptor().get_heap_handle(),
				.materialSrv = ctx.graph->get<ShaderResourceView>(*handles.material_srv.second)->allocate_online_descriptor().get_heap_handle(),
				.depthSrv = ctx.graph->get<ShaderResourceView>(*handles.depth_srv.second)->allocate_online_descriptor().get_heap_handle(),
				.framebufferUav = ctx.graph->get<UnorderedAccessView>(*handles.framebuffer_uav.second)->allocate_online_descriptor().get_heap_handle()
			};
			native->SetPipelineState(*PSO);;			
			native->SetComputeRootSignature(*g_RHI.rootSig);
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->GetGlobalsBuffer().GetGPUAddress());
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, sceneView->GetShadingBuffer().GetGPUAddress());
			native->SetComputeRoot32BitConstants(RHIContext::ROOTSIG_CB_8_CONSTANTS, 5, &constants, 0);
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});
}
