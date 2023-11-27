#include "IndirectLODCull.hpp"
using namespace RHI;
IndirectLODCullPass::IndirectLODCullPass(Device* device) {
	cullPassEarlyCS = BuildShader(L"IndirectLODCull", L"main_early", L"cs_6_6");
	cullPassLateCS = BuildShader(L"IndirectLODCull", L"main_late", L"cs_6_6");
	cullPassRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : SceneGlobals	
		.AddShaderResourceView(0, 0) // t0 space0 : SceneMeshInstance		
		.AddUnorderedAccessView(0, 0) // u0 space0 : Instance visibility
		.AddConstant(1,0,2) // b1 space0 : indirect CMD UAV heap, transparency indirect CMD UAV heap		
		.AddConstant(2,0,2) // b2 space0 : HIZ srv heap handle, HIZ mips
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
void IndirectLODCullPass::insert_execute(RenderGraphPass& pass, SceneView* sceneView, auto&& handles) {
	pass.execute([=](RgContext& ctx) -> void {
		auto* r_visibility_buffer = ctx.graph->get<Buffer>(handles.visibilityBuffer);
		auto* r_indirect_cmd_buffer = ctx.graph->get<Buffer>(handles.indirectCmdBuffer);
		auto* r_indirect_cmd_buffer_uav = ctx.graph->get<UnorderedAccessView>(handles.indirectCmdBufferUAV);		
		auto native = ctx.cmd->GetNativeCommandList();
		if constexpr (std::is_same_v<decltype(handles), IndirectLODLateCullPassHandles&>) {	
			auto* r_transparency_indirect_cmd_buffer = ctx.graph->get<Buffer>(handles.transparencyIndirectCmdBuffer);
			auto* r_transparency_indirect_cmd_buffer_uav = ctx.graph->get<UnorderedAccessView>(handles.transparencyIndirectCmdBufferUAV);
			native->SetPipelineState(*cullPassLatePSO);
			native->SetComputeRootSignature(*cullPassRS);
			auto* r_hiz_srv = ctx.graph->get<ShaderResourceView>(handles.hizSRV);
			auto* r_hiz = ctx.graph->get<Texture>(handles.hizTexture);
			native->SetComputeRoot32BitConstant(3, r_indirect_cmd_buffer_uav->descriptor.get_heap_handle(), 0);
			native->SetComputeRoot32BitConstant(3, r_transparency_indirect_cmd_buffer_uav->descriptor.get_heap_handle(), 1);
			native->SetComputeRoot32BitConstant(4, r_hiz_srv->descriptor.get_heap_handle(), 0);
			native->SetComputeRoot32BitConstant(4, r_hiz->GetDesc().mipLevels, 1);
			ctx.cmd->QueueTransitionBarrier(r_transparency_indirect_cmd_buffer, ResourceState::CopyDest);
			ctx.cmd->FlushBarriers();
			ctx.cmd->ZeroBufferRegion(r_transparency_indirect_cmd_buffer, CommandBufferCounterOffset, sizeof(UINT));
			ctx.cmd->QueueTransitionBarrier(r_transparency_indirect_cmd_buffer, ResourceState::UnorderedAccess);
		}
		else {
			native->SetPipelineState(*cullPassEarlyPSO);
			native->SetComputeRootSignature(*cullPassRS);
			native->SetComputeRoot32BitConstant(3, r_indirect_cmd_buffer_uav->descriptor.get_heap_handle(), 0);
		}
		native->SetComputeRootConstantBufferView(0, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
		native->SetComputeRootShaderResourceView(1, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
		native->SetComputeRootUnorderedAccessView(2, r_visibility_buffer->GetGPUAddress()); // u0 space0
		ctx.cmd->QueueTransitionBarrier(r_indirect_cmd_buffer, ResourceState::CopyDest);
		ctx.cmd->FlushBarriers();
		ctx.cmd->ZeroBufferRegion(r_indirect_cmd_buffer, CommandBufferCounterOffset, sizeof(UINT));
		ctx.cmd->QueueTransitionBarrier(r_indirect_cmd_buffer, ResourceState::UnorderedAccess);
		ctx.cmd->FlushBarriers();
		// dispatch compute to cull on the gpu
		native->Dispatch(DivRoundUp(sceneView->get_SceneGlobals().numMeshInstances, RENDERER_INSTANCE_CULL_THREADS), 1, 1);
	});
}
// xxx decided when to actually perform the clear
RenderGraphPass& IndirectLODCullPass::insert_clear(RenderGraph& rg, SceneView* sceneView, IndirectLODClearPassHandles&& handles) {
	return rg.add_pass(L"Indirect Cull Clear Visiblity Buffer")
		.readwrite(handles.visibilityBuffer)
		.execute([=](RgContext& ctx) -> void {
			auto* r_visibility_buffer = ctx.graph->get<Buffer>(handles.visibilityBuffer);
			auto state_prev = r_visibility_buffer->GetSubresourceState();
			ctx.cmd->QueueTransitionBarrier(r_visibility_buffer, ResourceState::CopyDest);
			ctx.cmd->FlushBarriers();			
			ctx.cmd->ZeroBufferRegion(r_visibility_buffer, 0, r_visibility_buffer->GetDesc().sizeInBytes());
			ctx.cmd->QueueTransitionBarrier(r_visibility_buffer, state_prev);
			ctx.cmd->FlushBarriers();
	});
}
RenderGraphPass& IndirectLODCullPass::insert_earlycull(RenderGraph& rg, SceneView* sceneView, IndirectLODEarlyCullPassHandles&& handles) {
	auto& pass = rg.add_pass(L"Early Indirect Cull & LOD") // { mesh | mesh.visiblePrevFrame && mesh.frustumCullPassed }
		.readwrite(handles.visibilityBuffer)
		.readwrite(handles.indirectCmdBuffer);
	insert_execute(pass, sceneView, handles);
	return pass;
}

RenderGraphPass& IndirectLODCullPass::insert_latecull(RenderGraph& rg, SceneView* sceneView, IndirectLODLateCullPassHandles&& handles) {
	auto& pass = rg.add_pass(L"Late Indirect Cull & LOD") // { mesh | !mesh.visiblePrevFrame && mesh.frustumCullPassed && mesh.occlusionCullPassed }, then sets visibility buffer
		.read(handles.hizTexture)
		.readwrite(handles.visibilityBuffer)
		.readwrite(handles.indirectCmdBuffer)
		.readwrite(handles.transparencyIndirectCmdBuffer);
	insert_execute(pass, sceneView, handles);
	return pass;
}
