#include "IndirectLODCull.hpp"
using namespace RHI;
IndirectLODCullPass::IndirectLODCullPass(Device* device) {
	cullPassEarlyCS = std::make_unique<Shader>(L"Shaders/IndirectLODCull.hlsl", L"main_early", L"cs_6_6");
	cullPassLateCS = std::make_unique<Shader>(L"Shaders/IndirectLODCull.hlsl", L"main_late", L"cs_6_6");
	cullPassRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : SceneGlobals	
		.AddShaderResourceView(0, 0) // t0 space0 : SceneMeshInstance
		.AddUnorderedAccessViewWithCounter(0, 0)// u0 space0 : Indirect Commandlists
		.AddUnorderedAccessView(1, 0) // u1 space0 : Instance Visibility
		.AddConstant(1,0,2) // b1 space0 : HIZ srv heap handle, HIZ mips
		.AddStaticSampler(0,0, SamplerDesc::GetDepthReduceSamplerDesc(
#ifdef INVERSE_Z
			true
#else
			false
#endif
		)) // s0 space0 : POINT sampler. MIN reduce if inverse Z. MAX otherwise
	);
	cullPassRS->SetName(L"Indirect Cull & LOD Classification");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *cullPassRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cullPassEarlyCS->GetData(), cullPassEarlyCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	cullPassEarlyPSO = std::make_unique<PipelineState>(device, std::move(pso));	
	cullPassEarlyPSO->SetName(L"Early Cull");
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cullPassLateCS->GetData(), cullPassLateCS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	cullPassLatePSO = std::make_unique<PipelineState>(device, std::move(pso));	
	cullPassLatePSO->SetName(L"Late Cull");
}
void IndirectLODCullPass::insert_execute(RenderGraphPass& pass, SceneGraphView* sceneView, auto&& handles) {
	pass.execute([=](RgContext& ctx) -> void {
		auto* r_visibility_buffer = ctx.graph->get<Buffer>(handles.visibilityBuffer);
		auto* r_indirect_cmd_buffer = ctx.graph->get<Buffer>(handles.indirectCmdBuffer);
		auto* r_indirect_cmd_buffer_uav = ctx.graph->get<UnorderedAccessView>(handles.indirectCmdBufferUAV);		
		auto native = ctx.cmd->GetNativeCommandList();
		if constexpr (std::is_same_v<decltype(handles), IndirectLODLateCullPassHandles&>) {			
			native->SetPipelineState(*cullPassLatePSO);
			native->SetComputeRootSignature(*cullPassRS);
			auto* r_hiz_srv = ctx.graph->get<ShaderResourceView>(handles.hizSRV);
			auto* r_hiz = ctx.graph->get<Texture>(handles.hizTexture);
			native->SetComputeRoot32BitConstant(4, r_hiz_srv->descriptor.get_heap_handle(), 0); // b1 space0
			native->SetComputeRoot32BitConstant(4, r_hiz->GetDesc().mipLevels, 1); // b1 space0
		}
		else {
			native->SetPipelineState(*cullPassEarlyPSO);
			native->SetComputeRootSignature(*cullPassRS);
		}
		native->SetComputeRootConstantBufferView(0, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
		native->SetComputeRootShaderResourceView(1, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
		native->SetComputeRootDescriptorTable(2, r_indirect_cmd_buffer_uav->descriptor);
		native->SetComputeRootUnorderedAccessView(3, r_visibility_buffer->GetGPUAddress()); // b0 space0			
		r_indirect_cmd_buffer->SetBarrier(ctx.cmd, ResourceState::CopyDest);
		ctx.cmd->ZeroBufferRegion(r_indirect_cmd_buffer, CommandBufferCounterOffset, sizeof(UINT));
		r_indirect_cmd_buffer->SetBarrier(ctx.cmd, ResourceState::UnorderedAccess);
		// dispatch compute to cull on the gpu
		native->Dispatch(DivRoundUp(sceneView->get_SceneGlobals().numMeshInstances, RENDERER_INSTANCE_CULL_THREADS), 1, 1);
	});
}
// xxx decided when to actually perform the clear
RenderGraphPass& IndirectLODCullPass::insert_clear(RenderGraph& rg, SceneGraphView* sceneView, IndirectLODClearPassHandles&& handles) {
	return rg.add_pass(L"Indirect Cull Clear Visiblity Buffer")
		.readwrite(handles.visibilityBuffer)
		.execute([=](RgContext& ctx) -> void {
			auto* r_visibility_buffer = ctx.graph->get<Buffer>(handles.visibilityBuffer);
			auto state_prev = r_visibility_buffer->GetState();
			r_visibility_buffer->SetBarrier(ctx.cmd, ResourceState::CopyDest);
			ctx.cmd->ZeroBufferRegion(r_visibility_buffer, 0, r_visibility_buffer->GetDesc().sizeInBytes());
			r_visibility_buffer->SetBarrier(ctx.cmd, state_prev);
	});
}
RenderGraphPass& IndirectLODCullPass::insert_earlycull(RenderGraph& rg, SceneGraphView* sceneView, IndirectLODEarlyCullPassHandles&& handles) {
	auto& pass = rg.add_pass(L"Early Indirect Cull & LOD") // { mesh | mesh.visiblePrevFrame && mesh.frustumCullPassed }
		.readwrite(handles.visibilityBuffer)
		.readwrite(handles.indirectCmdBuffer);
	insert_execute(pass, sceneView, handles);
	return pass;
}

RenderGraphPass& IndirectLODCullPass::insert_latecull(RenderGraph& rg, SceneGraphView* sceneView, IndirectLODLateCullPassHandles&& handles) {
	auto& pass = rg.add_pass(L"Late Indirect Cull & LOD") // { mesh | !mesh.visiblePrevFrame && mesh.frustumCullPassed && mesh.occlusionCullPassed }, then sets visibility buffer
		.read(handles.hizTexture)
		.readwrite(handles.visibilityBuffer)
		.readwrite(handles.indirectCmdBuffer);
	insert_execute(pass, sceneView, handles);
	return pass;
}
