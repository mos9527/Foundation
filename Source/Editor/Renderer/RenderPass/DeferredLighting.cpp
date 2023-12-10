#include "DeferredLighting.hpp"
#include "../../Processor/HDRIProbe.hpp"

using namespace RHI;
using namespace EditorGlobals;
void DeferredLightingPass::reset() {
	CS = ::BuildShader(L"DeferredLighting", L"main", L"cs_6_6");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *g_RHI.rootSig;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	constants = std::make_unique<BufferContainer<ShadingConstants>>(device, 1, L"Shading Constants");
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
			
			constants->Data()->framebufferUav = ctx.graph->get<UnorderedAccessView>(*handles.framebuffer_uav.second)->descriptor.get_heap_handle();
			constants->Data()->depthSrv = ctx.graph->get<ShaderResourceView>(*handles.depth_srv.second)->descriptor.get_heap_handle();
			constants->Data()->tangentFrameSrv = ctx.graph->get<ShaderResourceView>(*handles.tangentframe_srv.second)->descriptor.get_heap_handle();
			constants->Data()->gradientSrv = ctx.graph->get<ShaderResourceView>(*handles.gradient_srv.second)->descriptor.get_heap_handle();
			constants->Data()->materialSrv = ctx.graph->get<ShaderResourceView>(*handles.material_srv.second)->descriptor.get_heap_handle();		
			
			SceneIBLProbe probeHLSL{};
			{
				auto probe = sceneView->get_shader_data().probe.probe;
				probeHLSL.enabled = sceneView->get_shader_data().probe.use;
				probeHLSL.cubemapHeapIndex = probe->cubemapSRV->descriptor.get_heap_handle();
				probeHLSL.radianceHeapIndex = probe->radianceCubeArraySRV->descriptor.get_heap_handle();
				probeHLSL.irradianceHeapIndex = probe->irridanceCubeSRV->descriptor.get_heap_handle();
				probeHLSL.lutHeapIndex = probe->lutArraySRV->descriptor.get_heap_handle();
				probeHLSL.mips = probe->numMips;
				probeHLSL.diffuseIntensity = sceneView->get_shader_data().probe.diffuseIntensity;
				probeHLSL.specularIntensity = sceneView->get_shader_data().probe.specularIntensity;
				probeHLSL.occlusionStrength = sceneView->get_shader_data().probe.occlusionStrength;
			}
			constants->Data()->iblProbe = probeHLSL;

			native->SetPipelineState(*PSO);;			
			native->SetComputeRootSignature(*g_RHI.rootSig);
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->get_editor_globals_buffer()->GetGPUAddress());
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});
}
