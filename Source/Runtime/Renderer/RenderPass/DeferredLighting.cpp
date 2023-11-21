#include "DeferredLighting.hpp"
using namespace RHI;

DeferredLightingPass::DeferredLightingPass(Device* device) {
	lightingCS = std::make_unique<Shader>(L"Shaders/Lighting.hlsl", L"main", L"cs_6_6");
	lightingRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : SceneGlobals	
		.AddConstant(1, 0, 6) // b1 space0 : MRT handles + FB UAV
		.AddShaderResourceView(0, 0) // s0 space0 : Lights
	);
	lightingRS->SetName(L"Lighting");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *lightingRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(lightingCS->GetData(), lightingCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	lightingPSO = std::make_unique<PipelineState>(device, std::move(pso));
}

RenderGraphPass& DeferredLightingPass::insert(RenderGraph& rg, SceneGraphView* sceneView, DeferredLightingPassHandles& handles) {
	return rg.add_pass(L"Lighting")
		.readwrite(handles.frameBuffer)
		.read(handles.depth).read(handles.albedo).read(handles.normal).read(handles.material).read(handles.emissive)
		.execute([=](RgContext& ctx) {
			UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*lightingPSO);;
			native->SetComputeRootSignature(*lightingRS);
			auto* r_albedo_srv = ctx.graph->get<ShaderResourceView>(handles.albedo_srv);
			auto* r_normal_srv = ctx.graph->get<ShaderResourceView>(handles.normal_srv);
			auto* r_material_srv = ctx.graph->get<ShaderResourceView>(handles.material_srv);
			auto* r_emissive_srv = ctx.graph->get<ShaderResourceView>(handles.emissive_srv);
			auto* r_depth_srv = ctx.graph->get<ShaderResourceView>(handles.depth_srv);
			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(handles.fb_uav);
			UINT mrtHandles[6] = {
				r_albedo_srv->descriptor.get_heap_handle(),
				r_normal_srv->descriptor.get_heap_handle(),
				r_material_srv->descriptor.get_heap_handle(),
				r_emissive_srv->descriptor.get_heap_handle(),
				r_depth_srv->descriptor.get_heap_handle(),
				r_fb_uav->descriptor.get_heap_handle()
			};
			native->SetComputeRootConstantBufferView(0, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->SetComputeRoot32BitConstants(1, 6, mrtHandles, 0);
			native->SetComputeRootShaderResourceView(2, sceneView->get_SceneLightBuffer()->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});
}